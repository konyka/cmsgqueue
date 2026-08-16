#define _POSIX_C_SOURCE 200809L
#include "cmq_route_tls_sess.h"
#include "cmq_tls.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

struct cmq_route_tls_sess {
    SSL *ssl;
};

cmq_route_tls_sess_t *cmq_route_tls_sess_create(int fd, void *ssl_ctx) {
    if (!ssl_ctx) return NULL;
    SSL_CTX *ctx = (SSL_CTX *)ssl_ctx;
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    int rc = SSL_do_handshake(ssl);
    if (rc != 1) {
        SSL_free(ssl);
        return NULL;
    }
    cmq_route_tls_sess_t *sess = malloc(sizeof(*sess));
    if (!sess) {
        SSL_free(ssl);
        return NULL;
    }
    sess->ssl = ssl;
    return sess;
}

ssize_t cmq_route_tls_sess_read(cmq_route_tls_sess_t *sess,
                                 int fd, void *buf, size_t len) {
    (void)fd;
    if (!sess || !sess->ssl) return -1;
    int n = SSL_read(sess->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;
    int err = SSL_get_error(sess->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    return 0;  /* clean EOF */
}

ssize_t cmq_route_tls_sess_write(cmq_route_tls_sess_t *sess,
                                  int fd, const void *buf, size_t len) {
    (void)fd;
    if (!sess || !sess->ssl) return -1;
    int n = SSL_write(sess->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;
    int err = SSL_get_error(sess->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
}

void cmq_route_tls_sess_destroy(cmq_route_tls_sess_t *sess) {
    if (!sess) return;
    if (sess->ssl) {
        SSL_shutdown(sess->ssl);
        SSL_free(sess->ssl);
    }
    free(sess);
}
