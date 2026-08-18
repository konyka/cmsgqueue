/* P1: nonblocking TLS handshake over a loopback socket pair. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_route_tls_sess.h"

#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define TLS_TEST_DIR "/tmp/cmq-test-f17-handshake"

static void __attribute__((constructor)) install_sigpipe_handler(void) {
    signal(SIGPIPE, SIG_IGN);
}

static SSL_CTX *make_server_ctx(void) {
    const SSL_METHOD *m = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(m);
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, TLS_TEST_DIR "/cert.pem",
                                      SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, TLS_TEST_DIR "/key.pem",
                                     SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static int g_server_ready;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static void *server_thread(void *arg) {
    int srv_fd = *(int *)arg;
    SSL_CTX *ctx = make_server_ctx();
    if (!ctx) return NULL;
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, srv_fd);
    SSL_set_accept_state(ssl);
    pthread_mutex_lock(&g_mu);
    g_server_ready = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    (void)SSL_do_handshake(ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(srv_fd);
    return NULL;
}

TEST(route_tls_sess, nonblocking_handshake_succeeds) {
    system("rm -rf " TLS_TEST_DIR " && mkdir -p " TLS_TEST_DIR);
    int rc = system("openssl req -x509 -newkey rsa:2048 -keyout " TLS_TEST_DIR "/key.pem -out " TLS_TEST_DIR "/cert.pem -days 1 -nodes -subj '/CN=localhost' 2>/dev/null");
    ASSERT_EQ(rc, 0);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sv[1]);

    pthread_mutex_lock(&g_mu);
    while (!g_server_ready) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);

    const SSL_METHOD *m = TLS_client_method();
    SSL_CTX *cctx = SSL_CTX_new(m);
    ASSERT_NOT_NULL(cctx);
    SSL_CTX_set_min_proto_version(cctx, TLS1_2_VERSION);

    cmq_route_tls_sess_t *sess = cmq_route_tls_sess_create(sv[0], cctx);
    ASSERT_NOT_NULL(sess);

    int hs = -2;
    for (int i = 0; i < 50 && hs != 1 && hs != -1; i++) {
        hs = cmq_route_tls_sess_handshake(sess);
        if (hs == 0) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }
    }
    ASSERT_EQ(hs, 1);

    cmq_route_tls_sess_destroy(sess);
    SSL_CTX_free(cctx);
    close(sv[0]);
    pthread_join(tid, NULL);
    system("rm -rf " TLS_TEST_DIR);
}

TEST_MAIN()