#define _POSIX_C_SOURCE 200809L
#include "cmq_route_tls.h"
#include "cmq_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* F17: minimal config-object implementation. Reuses the existing
 * TLS config storage but exposes a separate API namespace so the
 * route layer doesn't depend on the listener TLS internals. */
struct cmq_route_tls_config {
    char cert[512];
    char key[512];
    char ca[512];
};

cmq_route_tls_config_t *cmq_route_tls_config_create(void) {
    return calloc(1, sizeof(cmq_route_tls_config_t));
}

void cmq_route_tls_config_destroy(cmq_route_tls_config_t *cfg) {
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
