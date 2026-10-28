#define _POSIX_C_SOURCE 200809L
#include "cmq_route_tls.h"
#include "cmq_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CMQ_TLS_OPENSSL
#include <openssl/ssl.h>
#endif

/* F17: minimal config-object implementation. Reuses the existing
 * TLS config storage but exposes a separate API namespace so the
 * route layer doesn't depend on the listener TLS internals. */
struct cmq_route_tls_config {
    char cert[512];
    char key[512];
    char ca[512];
#ifdef CMQ_TLS_OPENSSL
    SSL_CTX *ssl_ctx;          /* lazily built on first get_ssl_ctx call */
#endif
};

cmq_route_tls_config_t *cmq_route_tls_config_create(void) {
    return calloc(1, sizeof(cmq_route_tls_config_t));
}

void cmq_route_tls_config_destroy(cmq_route_tls_config_t *cfg) {
    if (!cfg) return;
#ifdef CMQ_TLS_OPENSSL
    if (cfg->ssl_ctx) SSL_CTX_free(cfg->ssl_ctx);
#endif
    free(cfg);
}

int cmq_route_tls_set_cert(cmq_route_tls_config_t *cfg, const char *path) {
    if (!cfg || !path) return -1;
    snprintf(cfg->cert, sizeof(cfg->cert), "%s", path);
    return 0;
}

int cmq_route_tls_set_key(cmq_route_tls_config_t *cfg, const char *path) {
    if (!cfg || !path) return -1;
    snprintf(cfg->key, sizeof(cfg->key), "%s", path);
    return 0;
}

int cmq_route_tls_set_ca(cmq_route_tls_config_t *cfg, const char *path) {
    if (!cfg || !path) return -1;
    snprintf(cfg->ca, sizeof(cfg->ca), "%s", path);
    return 0;
}

int cmq_route_tls_configured(cmq_route_tls_config_t *cfg) {
    if (!cfg) return 0;
    return (cfg->cert[0] && cfg->key[0]) ? 1 : 0;
}

int cmq_route_tls_available(void) {
    /* F17: 1 when the TLS backend (cmq_tls) is real OpenSSL. */
    return cmq_tls_backend_secure();
}

#ifdef CMQ_TLS_OPENSSL
/* Build a fresh SSL_CTX for the route TLS config. Returns NULL on
 * failure. Caller does NOT own the returned pointer — it is cached
 * on cfg and freed by cmq_route_tls_config_destroy. */
SSL_CTX *cmq_route_tls_get_ssl_ctx(cmq_route_tls_config_t *cfg) {
    if (!cfg) return NULL;
    if (!cfg->cert[0] || !cfg->key[0]) return NULL;
    if (cfg->ssl_ctx) return cfg->ssl_ctx;
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cfg->cert,
                                      SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg->key,
                                     SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    cfg->ssl_ctx = ctx;
    return ctx;
}
#endif
