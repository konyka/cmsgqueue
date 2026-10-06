/* F17: TLS wire-up end-to-end.
 *
 * Pins the contract between cmq_route_pool, cmq_route_conn_t, and
 * cmq_route_tls_sess_t:
 *
 *   1. cmq_route_pool_set_tls_cfg installs an SSL_CTX source on the
 *      pool.
 *   2. cmq_route_attach_inbound creates a cmq_route_tls_sess_t for
 *      the slot when the pool has TLS configured.
 *   3. cmq_route_get_conn returns the live sess via the conn struct.
 *   4. cmq_route_broadcast writes go through SSL_write when the
 *      destination slot has a non-NULL sess; the peer observes
 *      plaintext on its SSL_read.
 *
 * Before the wire-up, sess is NULL on every conn slot. The broadcast
 * round-trip then sends cleartext where the peer expects ciphertext
 * and SSL_read errors out, so the recv file is empty.
 */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_route.h"
#include "cmq_route_tls.h"
#include "cmq_route_tls_sess.h"

#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define F17_DIR "/tmp/cmq-test-f17-wire"

static void __attribute__((constructor)) install_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}

static int generate_cert(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "rm -rf " F17_DIR " && mkdir -p " F17_DIR " && "
             "openssl req -x509 -newkey rsa:2048 -keyout " F17_DIR "/key.pem "
             "-out " F17_DIR "/cert.pem -days 1 -nodes "
             "-subj '/CN=localhost' 2>/dev/null");
    return system(cmd);
}

static void *server_loop(void *arg) {
    int fd = *(int *)arg;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_use_certificate_file(ctx, F17_DIR "/cert.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, F17_DIR "/key.pem", SSL_FILETYPE_PEM);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    /* Drive the server-side handshake in nonblocking-friendly steps.
       A blocking SSL_do_handshake works on the socketpair but blocks
       forever if the client never gets to drive its own handshake —
       so we drive SSL_do_handshake repeatedly with select. */
    int rc = -1;
    for (int i = 0; i < 200; i++) {
        rc = SSL_do_handshake(ssl);
        if (rc == 1) break;
        int err = SSL_get_error(ssl, rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) break;
        struct timeval tv = {0, 10000}; /* 10 ms */
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        select(fd + 1, &rfds, NULL, NULL, &tv);
    }
    if (rc != 1) goto out;
    uint8_t buf[256];
    int n = SSL_read(ssl, buf, sizeof(buf));
    if (n > 0) {
        FILE *f = fopen(F17_DIR "/recv.bin", "wb");
        if (f) { fwrite(buf, 1, n, f); fclose(f); }
    }
out:
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return NULL;
}

TEST(f17_wire, attach_inbound_populates_sess_when_tls_configured) {
    ASSERT_EQ(generate_cert(), 0);
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    /* attach_inbound requires the fd to be nonblocking (the pool does
       not set it for borrowed fds — that is the caller's job). The
       server thread also expects nonblocking on its side. */
    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);

    cmq_route_tls_config_t *cfg = cmq_route_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_route_tls_set_cert(cfg, F17_DIR "/cert.pem"), 0);
    ASSERT_EQ(cmq_route_tls_set_key(cfg, F17_DIR "/key.pem"), 0);

    cmq_route_pool_t *pool = cmq_route_pool_create(NULL);
    ASSERT_NOT_NULL(pool);
    cmq_route_pool_set_tls_cfg(pool, cfg);

    pthread_t tid;
    pthread_create(&tid, NULL, server_loop, &sv[1]);

    ASSERT_EQ(cmq_route_attach_inbound(pool, "node-a", sv[0]), 0);
    cmq_route_conn_t snap;
    ASSERT_EQ(cmq_route_get_conn(pool, "node-a", &snap), 0);
    ASSERT_NOT_NULL(snap.sess);

    int hs = -2;
    for (int i = 0; i < 200 && hs != 1 && hs != -1; i++) {
        hs = cmq_route_tls_sess_handshake(snap.sess);
        if (hs == 0) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }
    }
    ASSERT_EQ(hs, 1);

    ASSERT_EQ(cmq_route_mark_connected(pool, sv[0]), 0);

    static const uint8_t msg[] = "F17-broadcast-marker-AAA";
    size_t msg_len = sizeof(msg) - 1;
    size_t eagain = 0;
    size_t sent = cmq_route_broadcast(pool, msg, msg_len, NULL, &eagain);
    ASSERT_EQ(sent, (size_t)1);
    ASSERT_EQ(eagain, (size_t)0);

    pthread_join(tid, NULL);

    FILE *f = fopen(F17_DIR "/recv.bin", "rb");
    ASSERT_NOT_NULL(f);
    uint8_t got[64] = {0};
    size_t n = fread(got, 1, sizeof(got), f);
    fclose(f);
    ASSERT_EQ(n, msg_len);
    ASSERT_MEM_EQ(got, msg, msg_len);

    cmq_route_pool_destroy(pool);
    cmq_route_tls_config_destroy(cfg);
}

TEST_MAIN()
