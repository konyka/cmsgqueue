#define _POSIX_C_SOURCE 200809L
#include "cmq_tls.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

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
};

struct cmq_tls_session {
    cmq_tls_config_t *cfg;
    int fd;
    int is_server;
    int handshake_done;
};

cmq_tls_config_t *cmq_tls_config_create(void) {
    cmq_tls_config_t *cfg = calloc(1, sizeof(cmq_tls_config_t));
    return cfg;
}

void cmq_tls_config_destroy(cmq_tls_config_t *cfg) {
    free(cfg);
}

int cmq_tls_set_cert(cmq_tls_config_t *cfg, const char *cert_path) {
    if (!cfg || !cert_path) return -1;
    strncpy(cfg->cert, cert_path, CMQ_TLS_PATH_MAX - 1);
    cfg->has_cert = 1;
    return 0;
}

int cmq_tls_set_key(cmq_tls_config_t *cfg, const char *key_path) {
    if (!cfg || !key_path) return -1;
    strncpy(cfg->key, key_path, CMQ_TLS_PATH_MAX - 1);
    cfg->has_key = 1;
    return 0;
}

int cmq_tls_set_ca(cmq_tls_config_t *cfg, const char *ca_path) {
    if (!cfg || !ca_path) return -1;
    strncpy(cfg->ca, ca_path, CMQ_TLS_PATH_MAX - 1);
    return 0;
}

int cmq_tls_set_verify(cmq_tls_config_t *cfg, int verify_peer) {
    if (!cfg) return -1;
    cfg->verify_peer = verify_peer;
    return 0;
}

int cmq_tls_set_server_name(cmq_tls_config_t *cfg, const char *name) {
    if (!cfg || !name) return -1;
    strncpy(cfg->server_name, name, CMQ_TLS_NAME_MAX - 1);
    return 0;
}

const char *cmq_tls_cert_path(cmq_tls_config_t *cfg) {
    return cfg ? cfg->cert : NULL;
}
const char *cmq_tls_key_path(cmq_tls_config_t *cfg) {
    return cfg ? cfg->key : NULL;
}
const char *cmq_tls_ca_path(cmq_tls_config_t *cfg) {
    return cfg ? cfg->ca : NULL;
}
int cmq_tls_verify_peer(cmq_tls_config_t *cfg) {
    return cfg ? cfg->verify_peer : 0;
}
const char *cmq_tls_server_name(cmq_tls_config_t *cfg) {
    return cfg ? cfg->server_name : NULL;
}

int cmq_tls_configured(cmq_tls_config_t *cfg) {
    return cfg && cfg->has_cert && cfg->has_key;
}

cmq_tls_session_t *cmq_tls_server_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0) return NULL;
    cmq_tls_session_t *s = calloc(1, sizeof(cmq_tls_session_t));
    if (!s) return NULL;
    s->cfg = cfg;
    s->fd = fd;
    s->is_server = 1;
    s->handshake_done = 0;
    return s;
}

cmq_tls_session_t *cmq_tls_client_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0) return NULL;
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
    if (session->fd >= 0) close(session->fd);
    free(session);
}

int cmq_tls_handshake(cmq_tls_session_t *session) {
    if (!session) return -1;
    session->handshake_done = 1;
    return 0;
}

ssize_t cmq_tls_read(cmq_tls_session_t *session, uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0) return -1;
    return read(session->fd, buf, len);
}

ssize_t cmq_tls_write(cmq_tls_session_t *session, const uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0) return -1;
    return write(session->fd, buf, len);
}

int cmq_tls_fd(cmq_tls_session_t *session) {
    return session ? session->fd : -1;
}
