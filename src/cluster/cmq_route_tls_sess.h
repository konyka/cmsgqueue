#ifndef CMQ_ROUTE_TLS_SESS_H
#define CMQ_ROUTE_TLS_SESS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F17: route TLS session wrapper.
 *
 * cmq_route_tls_sess_create allocates an SSL session over the given
 * fd using the config's SSL_CTX. Returns NULL if TLS is disabled
 * or the handshake fails.
 *
 * cmq_route_tls_sess_read / write: if sess is NULL, behave as
 * plain read/write. Otherwise, route through SSL_read / SSL_write.
 *
 * cmq_route_tls_sess_destroy frees the SSL session and shuts down
 * the TLS connection cleanly.
 */

typedef struct cmq_route_tls_sess cmq_route_tls_sess_t;

cmq_route_tls_sess_t *cmq_route_tls_sess_create(int fd, void *ssl_ctx);

ssize_t cmq_route_tls_sess_read(cmq_route_tls_sess_t *sess,
                                 int fd, void *buf, size_t len);

ssize_t cmq_route_tls_sess_write(cmq_route_tls_sess_t *sess,
                                  int fd, const void *buf, size_t len);

void cmq_route_tls_sess_destroy(cmq_route_tls_sess_t *sess);

#ifdef __cplusplus
}
#endif

#endif
