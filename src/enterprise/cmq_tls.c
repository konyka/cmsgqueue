#define _POSIX_C_SOURCE 200809L
#include "cmq_tls.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

#define CMQ_TLS_PATH_MAX 512
#define CMQ_TLS_NAME_MAX 256

struct cmq_tls_config {
    char cert[CMQ_TLS_PATH_MAX];
    char key[CMQ_TLS_PATH_MAX];
    char ca[CMQ_TLS_PATH_MAX];
    char server_name[CMQ_TLS_NAME_MAX];
    int verify_peer;
    int has_cert;
    int has_key;
    atomic_int in_flight;
    atomic_int dying;
};

struct cmq_tls_session {
    cmq_tls_config_t *cfg;
    int fd;
    int is_server;
    int handshake_done;
};

static int tls_begin_op(cmq_tls_config_t *cfg) {
    if (atomic_load_explicit(&cfg->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&cfg->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&cfg->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&cfg->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void tls_end_op(cmq_tls_config_t *cfg) {
    atomic_fetch_sub_explicit(&cfg->in_flight, 1, memory_order_acq_rel);
}

/* begin_op before any field touch — caller must not pass interior pointers. */
static int tls_copy_field(cmq_tls_config_t *cfg, size_t field_off, size_t field_cap,
                           char *out, size_t out_len) {
    if (!cfg || !out || out_len == 0) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    const char *field = (const char *)cfg + field_off;
    size_t n = strnlen(field, field_cap);
    if (n + 1 > out_len) {
        tls_end_op(cfg);
        return -1;
    }
    memcpy(out, field, n);
    out[n] = '\0';
    tls_end_op(cfg);
    return 0;
}

cmq_tls_config_t *cmq_tls_config_create(void) {
    cmq_tls_config_t *cfg = calloc(1, sizeof(cmq_tls_config_t));
    if (!cfg) return NULL;
    atomic_init(&cfg->in_flight, 0);
    atomic_init(&cfg->dying, 0);
    return cfg;
}

void cmq_tls_config_destroy(cmq_tls_config_t *cfg) {
    if (!cfg) return;
    atomic_store_explicit(&cfg->dying, 1, memory_order_release);
    while (atomic_load_explicit(&cfg->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    free(cfg);
}

int cmq_tls_set_cert(cmq_tls_config_t *cfg, const char *cert_path) {
    if (!cfg || !cert_path) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    strncpy(cfg->cert, cert_path, CMQ_TLS_PATH_MAX - 1);
    cfg->cert[CMQ_TLS_PATH_MAX - 1] = '\0';
    cfg->has_cert = 1;
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_set_key(cmq_tls_config_t *cfg, const char *key_path) {
    if (!cfg || !key_path) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    strncpy(cfg->key, key_path, CMQ_TLS_PATH_MAX - 1);
    cfg->key[CMQ_TLS_PATH_MAX - 1] = '\0';
    cfg->has_key = 1;
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_set_ca(cmq_tls_config_t *cfg, const char *ca_path) {
    if (!cfg || !ca_path) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    strncpy(cfg->ca, ca_path, CMQ_TLS_PATH_MAX - 1);
    cfg->ca[CMQ_TLS_PATH_MAX - 1] = '\0';
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_set_verify(cmq_tls_config_t *cfg, int verify_peer) {
    if (!cfg) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    cfg->verify_peer = verify_peer;
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_set_server_name(cmq_tls_config_t *cfg, const char *name) {
    if (!cfg || !name) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    strncpy(cfg->server_name, name, CMQ_TLS_NAME_MAX - 1);
    cfg->server_name[CMQ_TLS_NAME_MAX - 1] = '\0';
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_cert_path(cmq_tls_config_t *cfg, char *out, size_t out_len) {
    return tls_copy_field(cfg, offsetof(struct cmq_tls_config, cert),
                          CMQ_TLS_PATH_MAX, out, out_len);
}
int cmq_tls_key_path(cmq_tls_config_t *cfg, char *out, size_t out_len) {
    return tls_copy_field(cfg, offsetof(struct cmq_tls_config, key),
                          CMQ_TLS_PATH_MAX, out, out_len);
}
int cmq_tls_ca_path(cmq_tls_config_t *cfg, char *out, size_t out_len) {
    return tls_copy_field(cfg, offsetof(struct cmq_tls_config, ca),
                          CMQ_TLS_PATH_MAX, out, out_len);
}
int cmq_tls_verify_peer(cmq_tls_config_t *cfg) {
    if (!cfg) return 0;
    if (tls_begin_op(cfg) != 0) return 0;
    int v = cfg->verify_peer;
    tls_end_op(cfg);
    return v;
}
int cmq_tls_server_name(cmq_tls_config_t *cfg, char *out, size_t out_len) {
    return tls_copy_field(cfg, offsetof(struct cmq_tls_config, server_name),
                          CMQ_TLS_NAME_MAX, out, out_len);
}

int cmq_tls_configured(cmq_tls_config_t *cfg) {
    if (!cfg) return 0;
    if (tls_begin_op(cfg) != 0) return 0;
    int ok = cfg->has_cert && cfg->has_key;
    tls_end_op(cfg);
    return ok;
}

int cmq_tls_backend_secure(void) {
    /* This translation unit is a plaintext stub — fail closed at the server. */
    return 0;
}

cmq_tls_session_t *cmq_tls_server_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0 || !cmq_tls_configured(cfg)) return NULL;
    cmq_tls_session_t *s = calloc(1, sizeof(cmq_tls_session_t));
    if (!s) return NULL;
    s->cfg = cfg;
    s->fd = fd;
    s->is_server = 1;
    s->handshake_done = 0;
    return s;
}

cmq_tls_session_t *cmq_tls_client_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0 || !cmq_tls_configured(cfg)) return NULL;
    cmq_tls_session_t *s = calloc(1, sizeof(cmq_tls_session_t));
    if (!s) return NULL;
    s->cfg = cfg;
    s->fd = fd;
    s->is_server = 0;
    s->handshake_done = 0;
    return s;
}

void cmq_tls_session_destroy(cmq_tls_session_t *session) {
    if (!session) return;
    /* Session does not own the fd — the client closes it once. */
    session->fd = -1;
    free(session);
}

int cmq_tls_handshake(cmq_tls_session_t *session) {
    if (!session) return -1;
    session->handshake_done = 1;
    return 0;
}

ssize_t cmq_tls_read(cmq_tls_session_t *session, uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0 || session->fd < 0 ||
        !session->handshake_done)
        return -1;
    return read(session->fd, buf, len);
}

ssize_t cmq_tls_write(cmq_tls_session_t *session, const uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0 || session->fd < 0 ||
        !session->handshake_done)
        return -1;
    return write(session->fd, buf, len);
}

int cmq_tls_fd(cmq_tls_session_t *session) {
    return session ? session->fd : -1;
}
