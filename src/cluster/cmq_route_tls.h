#ifndef CMQ_ROUTE_TLS_H
#define CMQ_ROUTE_TLS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F17: Inter-node encryption for cluster routes.
 *
 * SSL_CTX-based TLS wrapper used when connecting to a peer configured
 * with route_require_tls. The library is built on top of OpenSSL 3.x.
 *
 * The wiring in cmq_route.c is left as a follow-up because the
 * existing route code uses plain read/write syscalls. This module
 * provides the API surface and the configuration object; the actual
 * socket BIO-wrap is queued for v0.5.0 work where the route socket
 * is replaced with a TLS-aware variant.
 */

typedef struct cmq_route_tls_config cmq_route_tls_config_t;

cmq_route_tls_config_t *cmq_route_tls_config_create(void);
void cmq_route_tls_config_destroy(cmq_route_tls_config_t *cfg);

int cmq_route_tls_set_cert(cmq_route_tls_config_t *cfg, const char *path);
int cmq_route_tls_set_key(cmq_route_tls_config_t *cfg, const char *path);
int cmq_route_tls_set_ca(cmq_route_tls_config_t *cfg, const char *path);

/* Returns 1 if cert+key are loaded. */
int cmq_route_tls_configured(cmq_route_tls_config_t *cfg);

/* F17: returns 1 when OpenSSL is available (i.e., the build linked
 * against OpenSSL and the inter-node TLS module is functional). */
int cmq_route_tls_available(void);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_ROUTE_TLS_H */
