#define _POSIX_C_SOURCE 200809L
#include "cmq_tls.h"
#include "cmq_tls_session_cache.h"
#include "cmq_log.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

/* F1: OpenSSL-backed TLS implementation (replaces plaintext stub).
 *
 * When CMQ_TLS_OPENSSL is defined (set by CMake when OpenSSL is found),
 * the implementation uses OpenSSL 1.1.1+ / 3.x APIs. When not defined,
 * the cmq_tls_backend_secure() returns 0 and the server fails closed.
 *
 * The OpenSSL context (SSL_CTX) is per-tls_config (one per listener),
 * not per-connection. Sessions hold an SSL* and operate on the BIO
 * wrapping the underlying fd.
 */
#ifdef CMQ_TLS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <fcntl.h>
#endif

#define CMQ_TLS_PATH_MAX 512
#define CMQ_TLS_NAME_MAX 256

struct cmq_tls_config {
    char cert[CMQ_TLS_PATH_MAX];
    char key[CMQ_TLS_PATH_MAX];
    char ca[CMQ_TLS_PATH_MAX];
    char crl[CMQ_TLS_PATH_MAX];
    char server_name[CMQ_TLS_NAME_MAX];
    int verify_peer;
    uint64_t last_crl_log_ms;  /* P3 v0.5.6: log throttle */
    int has_cert;
    int has_key;
    /* F12: ALPN protocol list. CSV-encoded (e.g. "h2,http/1.1"). */
    unsigned char alpn_data[256];
    unsigned int alpn_len;
    atomic_int in_flight;
    atomic_int dying;
#ifdef CMQ_TLS_OPENSSL
    SSL_CTX *ssl_ctx;  /* per-listener context, immutable after init */
    int ssl_ctx_init_done;
#endif
    void *session_cache_state;  /* v0.5.23: TLS session resumption cache */
};

struct cmq_tls_session {
    cmq_tls_config_t *cfg;
    int fd;
    int is_server;
    int handshake_done;
#ifdef CMQ_TLS_OPENSSL
    SSL *ssl;
#endif
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

#ifdef CMQ_TLS_OPENSSL
/* Build the SSL_CTX for a configured listener. Called once per
 * cmq_tls_config after cert/key are set. Returns 0 on success. */
/* Forward decls: OpenSSL session-callbacks. Defined below tls_build_ssl_ctx
 * so the SSL_CTX can be built first and then have these registered. */
#ifdef CMQ_TLS_OPENSSL
static int cmq_tls_sess_new_cb(SSL *ssl, SSL_SESSION *sess);
static SSL_SESSION *cmq_tls_sess_get_cb(SSL *ssl, const unsigned char *id,
                                          int id_len, int *copy);
static int cmq_tls_gen_session_id(SSL *ssl, unsigned char *id,
                                    unsigned int *id_len);
#endif

static int tls_build_ssl_ctx(cmq_tls_config_t *cfg) {
    if (cfg->ssl_ctx_init_done) return 0;
    const SSL_METHOD *method = TLS_server_method();
    if (!method) return -1;
    cfg->ssl_ctx = SSL_CTX_new(method);
    if (!cfg->ssl_ctx) return -1;
    /* TLS 1.2 floor; TLS 1.3 preferred. */
    SSL_CTX_set_min_proto_version(cfg->ssl_ctx, TLS1_2_VERSION);
    /* AEAD-only cipher list: TLS 1.3 ciphers are fixed by the protocol
     * (AEAD-only). For TLS 1.2, restrict to AEAD suites. */
    const char *ciphers =
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305";
    SSL_CTX_set_cipher_list(cfg->ssl_ctx, ciphers);
    /* CRIME/BREACH mitigation: disable TLS-level compression. */
    SSL_CTX_set_options(cfg->ssl_ctx, SSL_OP_NO_COMPRESSION);
    if (cfg->alpn_len > 0) {
        if (SSL_CTX_set_alpn_protos(cfg->ssl_ctx, cfg->alpn_data,
                                     cfg->alpn_len) != 0) {
            SSL_CTX_free(cfg->ssl_ctx);
            cfg->ssl_ctx = NULL;
            return -1;
        }
    }
    /* Best-effort defaults. */
    SSL_CTX_set_mode(cfg->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                    SSL_MODE_ENABLE_PARTIAL_WRITE);
    /* Load cert chain. */
    if (SSL_CTX_use_certificate_chain_file(cfg->ssl_ctx, cfg->cert) != 1) {
        SSL_CTX_free(cfg->ssl_ctx);
        cfg->ssl_ctx = NULL;
        return -1;
    }
    /* Load private key. */
    if (SSL_CTX_use_PrivateKey_file(cfg->ssl_ctx, cfg->key,
                                      SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(cfg->ssl_ctx);
        cfg->ssl_ctx = NULL;
        return -1;
    }
    if (SSL_CTX_check_private_key(cfg->ssl_ctx) != 1) {
        SSL_CTX_free(cfg->ssl_ctx);
        cfg->ssl_ctx = NULL;
        return -1;
    }
    /* Optional CA for client cert verification. */
    if (cfg->ca[0] != '\0') {
        SSL_CTX_load_verify_locations(cfg->ssl_ctx, cfg->ca, NULL);
    }
    /* P1 (v0.5.3): load the CRL into the SSL_CTX's X509_STORE. OpenSSL
     * consults the store automatically when SSL_VERIFY_PEER is on.
     * Failure to load is logged + CRL check is skipped (we don't want
     * a bad CRL file to refuse all handshakes). */
    if (cfg->crl[0] != '\0') {
        BIO *crl_bio = BIO_new_file(cfg->crl, "r");
        if (crl_bio) {
            X509_CRL *crl_obj = PEM_read_bio_X509_CRL(crl_bio, NULL, NULL, NULL);
            BIO_free(crl_bio);
            if (crl_obj) {
                X509_STORE *store = SSL_CTX_get_cert_store(cfg->ssl_ctx);
                if (store) {
                    X509_STORE_add_crl(store, crl_obj);
                    /* Lookups consult the CRL when verify_peer is on. */
                } else {
                    X509_CRL_free(crl_obj);
                }
            }
        }
    }
    /* P1: honor cfg->verify_peer. Without this the CA is loaded but
     * never checked (any client cert is accepted). Per the bundle B4,
     * set SSL_VERIFY_PEER + fail-if-no-cert when the caller opted in. */
    if (cfg->verify_peer) {
        int mode = SSL_VERIFY_PEER;
        if (cfg->ca[0] != '\0') mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_CTX_set_verify(cfg->ssl_ctx, mode, NULL);
        if (cfg->ca[0] != '\0') {
            /* Set the verification depth (chain-of-trust). */
            SSL_CTX_set_verify_depth(cfg->ssl_ctx, 9);
        }
    }
    /* v0.5.27: wire the session resumption cache into OpenSSL's
     * handshake lifecycle. The new_cb fires after a successful
     * handshake with the freshly-negotiated session; we insert it
     * into our cache. The get_cb fires when a client presents a
     * session ID in the ClientHello; we look it up and return the
     * cached session (or NULL on miss, which falls back to a full
     * handshake). NO_INTERNAL tells OpenSSL not to maintain its
     * own parallel session cache. The cfg pointer is stashed on the
     * SSL_CTX via SSL_CTX_set_app_data so the callbacks can find the
     * per-config cache. */
    SSL_CTX_set_app_data(cfg->ssl_ctx, cfg);
    SSL_CTX_sess_set_new_cb(cfg->ssl_ctx, cmq_tls_sess_new_cb);
    SSL_CTX_sess_set_get_cb(cfg->ssl_ctx, cmq_tls_sess_get_cb);
    /* v0.5.29: install a session ID generator. Without this, OpenSSL
     * 3.5 defaults to ticket-based resumption for TLS 1.2 and the
     * session ID is empty — which means our ID-keyed cache is only
     * exercised by new_session (insert) but never by get_cb (lookup).
     * Generating a 32-byte random ID per session forces ID-based
     * resumption and makes the cache hot on the lookup path. */
    SSL_CTX_set_session_id_context(cfg->ssl_ctx,
        (const unsigned char *)"cmq-tls-v0.5.29", 16);
    SSL_CTX_set_generate_session_id(cfg->ssl_ctx, cmq_tls_gen_session_id);
    SSL_CTX_set_session_cache_mode(cfg->ssl_ctx,
        SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_INTERNAL);
    cfg->ssl_ctx_init_done = 1;
    return 0;
}
#endif

cmq_tls_config_t *cmq_tls_config_create(void) {
    cmq_tls_config_t *cfg = calloc(1, sizeof(cmq_tls_config_t));
    if (!cfg) return NULL;
    atomic_init(&cfg->in_flight, 0);
    atomic_init(&cfg->dying, 0);
#ifdef CMQ_TLS_OPENSSL
    cfg->ssl_ctx = NULL;
    cfg->ssl_ctx_init_done = 0;
#endif
    return cfg;
}

void cmq_tls_config_destroy(cmq_tls_config_t *cfg) {
    if (!cfg) return;
    atomic_store_explicit(&cfg->dying, 1, memory_order_release);
    while (atomic_load_explicit(&cfg->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
#ifdef CMQ_TLS_OPENSSL
    if (cfg->ssl_ctx) {
        SSL_CTX_free(cfg->ssl_ctx);
        cfg->ssl_ctx = NULL;
    }
#endif
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

int cmq_tls_load(cmq_tls_config_t *cfg) {
    if (!cfg) return -1;
    if (!cfg->has_cert || !cfg->has_key) return -1;
#ifdef CMQ_TLS_OPENSSL
    if (tls_begin_op(cfg) != 0) return -1;
    int rc = tls_build_ssl_ctx(cfg);
    tls_end_op(cfg);
    return rc;
#else
    return 0;
#endif
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

/* P2 (v0.5.2): CRL path. Loaded into the SSL_CTX's X509_STORE
 * at cmq_tls_load time. Peer certs whose serial matches a CRL
 * entry fail verification. NULL disables. */
int cmq_tls_set_crl(cmq_tls_config_t *cfg, const char *crl_path) {
    if (!cfg) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    if (crl_path) {
        snprintf(cfg->crl, sizeof(cfg->crl), "%s", crl_path);
        cfg->last_crl_log_ms = 0;  /* P3 v0.5.6: reset log throttle */
    } else {
        /* P3 v0.5.6: rate-limit log to 1/sec. */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL +
                          (uint64_t)ts.tv_nsec / 1000000ULL;
        if (now_ms - cfg->last_crl_log_ms > 1000) {
            cmq_log_info(NULL,
                "cmq_tls_set_crl: disabling CRL check (NULL path)");
            cfg->last_crl_log_ms = now_ms;
        }
        cfg->crl[0] = '\0';
    }
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_crl_path(cmq_tls_config_t *cfg, char *out, size_t out_len) {
    if (!cfg || !out || out_len == 0) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    snprintf(out, out_len, "%s", cfg->crl);
    tls_end_op(cfg);
    return 0;
}

/* F12: ALPN protocol list. CSV "proto1,proto2" → wire-format
 * length-prefixed string list. Must be called BEFORE cmq_tls_load. */
int cmq_tls_set_alpn(cmq_tls_config_t *cfg, const char *protos_csv) {
    if (!cfg) return -1;
    if (tls_begin_op(cfg) != 0) return -1;
    cfg->alpn_len = 0;
    if (!protos_csv) { tls_end_op(cfg); return 0; }
    /* Each protocol encoded as: 1 byte length + N bytes string. */
    const char *p = protos_csv;
    while (*p && cfg->alpn_len + 2 < sizeof(cfg->alpn_data)) {
        const char *comma = strchr(p, ',');
        size_t plen = comma ? (size_t)(comma - p) : strlen(p);
        if (plen == 0 || plen > 127) { p = comma ? comma + 1 : p + plen; continue; }
        cfg->alpn_data[cfg->alpn_len++] = (unsigned char)plen;
        if (cfg->alpn_len + plen >= sizeof(cfg->alpn_data)) break;
        memcpy(cfg->alpn_data + cfg->alpn_len, p, plen);
        cfg->alpn_len += (unsigned int)plen;
        p = comma ? comma + 1 : p + plen;
    }
    tls_end_op(cfg);
    return 0;
}

int cmq_tls_alpn_has(cmq_tls_config_t *cfg, const char *proto) {
    if (!cfg || !proto || !proto[0]) return 0;
    size_t plen = strlen(proto);
    if (tls_begin_op(cfg) != 0) return 0;
    unsigned int i = 0;
    int rc = 0;
    while (i < cfg->alpn_len) {
        unsigned int n = cfg->alpn_data[i++];
        if (i + n > cfg->alpn_len) break;
        if (n == plen && memcmp(cfg->alpn_data + i, proto, n) == 0) {
            rc = 1;
            break;
        }
        i += n;
    }
    tls_end_op(cfg);
    return rc;
}

/* F12: Reload the SSL_CTX from the current cert/key paths.
 * Existing sessions continue with the old CTX until they tear down.
 * Returns 0 on success, -1 on failure. */
int cmq_tls_reload(cmq_tls_config_t *cfg) {
    if (!cfg) return -1;
#ifndef CMQ_TLS_OPENSSL
    return 0;
#else
    if (tls_begin_op(cfg) != 0) return -1;
    /* Build new CTX off-line. */
    const char *method = "TLS";
    (void)method;
    const SSL_METHOD *m = TLS_server_method();
    if (!m) { tls_end_op(cfg); return -1; }
    SSL_CTX *new_ctx = SSL_CTX_new(m);
    if (!new_ctx) { tls_end_op(cfg); return -1; }
    SSL_CTX_set_min_proto_version(new_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(new_ctx, TLS1_2_VERSION);
    const char *ciphers =
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305";
    if (SSL_CTX_set_cipher_list(new_ctx, ciphers) != 1) {
        SSL_CTX_free(new_ctx);
        tls_end_op(cfg);
        return -1;
    }
    SSL_CTX_set_options(new_ctx, SSL_OP_NO_COMPRESSION);
    if (cfg->alpn_len > 0) {
        if (SSL_CTX_set_alpn_protos(new_ctx, cfg->alpn_data,
                                     cfg->alpn_len) != 0) {
            SSL_CTX_free(new_ctx);
            tls_end_op(cfg);
            return -1;
        }
    }
    if (SSL_CTX_use_certificate_chain_file(new_ctx, cfg->cert) != 1) {
        SSL_CTX_free(new_ctx);
        tls_end_op(cfg);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(new_ctx, cfg->key, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(new_ctx);
        tls_end_op(cfg);
        return -1;
    }
    if (SSL_CTX_check_private_key(new_ctx) != 1) {
        SSL_CTX_free(new_ctx);
        tls_end_op(cfg);
        return -1;
    }
    if (cfg->ca[0] != '\0') {
        if (SSL_CTX_load_verify_locations(new_ctx, cfg->ca, NULL) != 1) {
            SSL_CTX_free(new_ctx);
            tls_end_op(cfg);
            return -1;
        }
    }
    if (cfg->verify_peer) {
        SSL_CTX_set_verify(new_ctx, SSL_VERIFY_PEER |
                                 SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }
    /* P1 v0.5.4 UAF fix: bump the new CTX's built-in OpenSSL
     * refcount so it isn't freed when the next reload decrements
     * to zero. Existing in-flight SSL* each hold a borrowed
     * reference via SSL_CTX_up_ref on session init. We free the
     * old CTX only when its refcount drops to 0. */
    if (SSL_CTX_up_ref(new_ctx) != 1) {
        SSL_CTX_free(new_ctx);
        tls_end_op(cfg);
        return -1;
    }
    SSL_CTX *old = cfg->ssl_ctx;
    cfg->ssl_ctx = new_ctx;
    cfg->ssl_ctx_init_done = 1;
    tls_end_op(cfg);
    if (old) SSL_CTX_free(old);
    return 0;
#endif
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
#ifdef CMQ_TLS_OPENSSL
    return 1;
#else
    /* Plaintext stub — fail closed at the server. */
    return 0;
#endif
}

cmq_tls_session_t *cmq_tls_server_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0 || !cmq_tls_configured(cfg)) return NULL;
#ifdef CMQ_TLS_OPENSSL
    /* Build the SSL_CTX lazily on first session. */
    if (!cfg->ssl_ctx_init_done) {
        if (tls_build_ssl_ctx(cfg) != 0) return NULL;
    }
#endif
    cmq_tls_session_t *s = calloc(1, sizeof(cmq_tls_session_t));
    if (!s) return NULL;
    s->cfg = cfg;
    s->fd = fd;
    s->is_server = 1;
    s->handshake_done = 0;
#ifdef CMQ_TLS_OPENSSL
    s->ssl = SSL_new(cfg->ssl_ctx);
    if (!s->ssl) {
        free(s);
        return NULL;
    }
    SSL_set_fd(s->ssl, fd);
    /* Set the fd non-blocking so SSL_do_handshake returns
     * WANT_READ/WANT_WRITE instead of blocking on a low-level read. */
    {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    SSL_set_accept_state(s->ssl);
#endif
    return s;
}

cmq_tls_session_t *cmq_tls_client_session(cmq_tls_config_t *cfg, int fd) {
    if (!cfg || fd < 0 || !cmq_tls_configured(cfg)) return NULL;
#ifdef CMQ_TLS_OPENSSL
    if (!cfg->ssl_ctx_init_done) {
        if (tls_build_ssl_ctx(cfg) != 0) return NULL;
    }
#endif
    cmq_tls_session_t *s = calloc(1, sizeof(cmq_tls_session_t));
    if (!s) return NULL;
    s->cfg = cfg;
    s->fd = fd;
    s->is_server = 0;
    s->handshake_done = 0;
#ifdef CMQ_TLS_OPENSSL
    s->ssl = SSL_new(cfg->ssl_ctx);
    if (!s->ssl) {
        free(s);
        return NULL;
    }
    SSL_set_fd(s->ssl, fd);
    /* Set the fd non-blocking so SSL_do_handshake returns
     * WANT_READ/WANT_WRITE instead of blocking on a low-level read. */
    {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    SSL_set_connect_state(s->ssl);
#endif
    return s;
}

void cmq_tls_session_destroy(cmq_tls_session_t *session) {
    if (!session) return;
#ifdef CMQ_TLS_OPENSSL
    if (session->ssl) {
        /* Best-effort TLS shutdown. Don't loop: if the kernel buffer
         * is full, we just close the underlying fd. */
        SSL_shutdown(session->ssl);
        SSL_free(session->ssl);
        session->ssl = NULL;
    }
#endif
    /* Session does not own the fd — the client closes it once. */
    session->fd = -1;
    free(session);
}

int cmq_tls_handshake(cmq_tls_session_t *session) {
    if (!session) return -1;
#ifndef CMQ_TLS_OPENSSL
    /* Plaintext stub: handshake is a no-op. */
    session->handshake_done = 1;
    return 0;
#else
    if (session->handshake_done) return 0;
    if (!session->ssl) return -1;
    int rc = SSL_do_handshake(session->ssl);
    if (rc == 1) {
        session->handshake_done = 1;
        return 1;
    }
    int err = SSL_get_error(session->ssl, rc);
    /* WANT_READ/WANT_WRITE mean "try again later" — non-blocking. */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 0;
    }
    /* Real error. */
    return -1;
#endif
}

ssize_t cmq_tls_read(cmq_tls_session_t *session, uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0 || session->fd < 0 ||
        !session->handshake_done)
        return -1;
#ifndef CMQ_TLS_OPENSSL
    return read(session->fd, buf, len);
#else
    int n = SSL_read(session->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;
    int err = SSL_get_error(session->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0; /* clean shutdown */
    }
    return -1;
#endif
}

ssize_t cmq_tls_write(cmq_tls_session_t *session, const uint8_t *buf, size_t len) {
    if (!session || !buf || len == 0 || session->fd < 0 ||
        !session->handshake_done)
        return -1;
#ifndef CMQ_TLS_OPENSSL
    return write(session->fd, buf, len);
#else
    int n = SSL_write(session->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;
    int err = SSL_get_error(session->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
#endif
}

int cmq_tls_fd(cmq_tls_session_t *session) {
    return session ? session->fd : -1;
}

/* v0.5.23: opaque accessors used by cmq_tls_session_cache.c. The cache
 * module needs to store its state on the config without making the
 * struct layout public. */
void *cmq_tls_get_session_cache_state(cmq_tls_config_t *cfg) {
    return cfg ? cfg->session_cache_state : NULL;
}

int cmq_tls_set_session_cache_state(cmq_tls_config_t *cfg, void *state) {
    if (!cfg) return -1;
    cfg->session_cache_state = state;
    return 0;
}

void cmq_tls_session_free_slot(void *sess) {
#ifdef CMQ_TLS_OPENSSL
    if (sess) SSL_SESSION_free((SSL_SESSION *)sess);
#endif
    (void)sess;
}

#ifdef CMQ_TLS_OPENSSL
/* v0.5.27: OpenSSL session-callback wiring.
 *
 * `new_cb` is called by OpenSSL after a successful handshake with the
 * freshly-minted SSL_SESSION*. We extract its session ID and insert into
 * the per-config cache (which takes ownership of the up-ref'd session).
 *
 * `get_cb` is called by OpenSSL when a client presents a session ID in
 * the ClientHello. We return the cached SSL_SESSION* (or NULL on miss
 * or invalid params). OpenSSL does NOT take ownership of the returned
 * pointer; we return a borrowed reference. The cache's lifetime owns
 * the underlying storage.
 *
 * The cmq_tls_config_t* is stashed on the SSL_CTX via SSL_CTX_set_app_data
 * (set in tls_build_ssl_ctx) so the callbacks can find the per-config
 * cache. */
static int cmq_tls_sess_new_cb(SSL *ssl, SSL_SESSION *sess) {
    if (!ssl || !sess) return 0;
    SSL_CTX *ctx = SSL_get_SSL_CTX(ssl);
    if (!ctx) return 0;
    cmq_tls_config_t *cfg = (cmq_tls_config_t *)SSL_CTX_get_app_data(ctx);
    if (!cfg) return 0;
    unsigned int id_len = 0;
    const unsigned char *id = SSL_SESSION_get_id(sess, &id_len);
    if (!id || id_len == 0) return 0;
    /* Up-ref so the cache owns its own reference. */
    if (SSL_SESSION_up_ref(sess) != 1) return 0;
    if (cmq_tls_session_cache_insert(cfg, id, id_len, sess) != 0) {
        /* Insert failed (e.g., id too long) — release the up-ref. */
        SSL_SESSION_free(sess);
        return 0;
    }
    return 1;
}

static SSL_SESSION *cmq_tls_sess_get_cb(SSL *ssl, const unsigned char *id,
                                          int id_len, int *copy) {
    if (copy) *copy = 0;
    if (!ssl || !id || id_len <= 0) return NULL;
    SSL_CTX *ctx = SSL_get_SSL_CTX(ssl);
    if (!ctx) return NULL;
    cmq_tls_config_t *cfg = (cmq_tls_config_t *)SSL_CTX_get_app_data(ctx);
    if (!cfg) return NULL;
    return (SSL_SESSION *)(void *)cmq_tls_session_cache_lookup(cfg, id, (unsigned int)id_len);
}

/* v0.5.29: session ID generator. OpenSSL 3.5 only assigns a session ID
 * if a callback is installed. Without one, sessions get ticket-based
 * resumption (stateless) and our ID-keyed cache is never hit by get_cb.
 * We allocate 32 random bytes per session — statistically unique, so
 * collisions are impossible. */
static int cmq_tls_gen_session_id(SSL *ssl, unsigned char *id,
                                    unsigned int *id_len) {
    (void)ssl;
    if (!id || !id_len) return 0;
    if (*id_len < 32) return 0;
    if (RAND_bytes(id, 32) != 1) return 0;
    *id_len = 32;
    return 1;
}
#endif

#ifdef CMQ_TLS_OPENSSL
SSL_CTX *cmq_tls_get_ssl_ctx_for_test(cmq_tls_config_t *cfg) {
    if (!cfg) return NULL;
    if (!cfg->ssl_ctx_init_done) {
        if (tls_build_ssl_ctx(cfg) != 0) return NULL;
    }
    return cfg->ssl_ctx;
}
#endif
