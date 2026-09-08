/* v0.5.45: end-to-end TLS handshake through cmq_server_run.
 *
 * Earlier v0.5.x added TLS handshaking helpers, the per-listener
 * SSL_CTX slots, the session cache, and the OpenSSL new_session /
 * gen_session_id callbacks. v0.5.28 verified a real handshake on a
 * synthetic socketpair — but it never drove cmq_server's accept
 * path. v0.5.45 is the missing link: spin up cmq_server_run with
 * TLS enabled, open a TCP socket, run SSL_connect, and verify the
 * server accepts and the handshake completes.
 *
 * Two tests:
 *   1. tls_e2e_handshake.single_listener
 *      One TLS listener; full handshake against port.
 *   2. tls_e2e_handshake.multi_listener_distinct_certs
 *      Two TLS listeners (slot 0 + slot 1) with distinct certs;
 *      verify port+0 trusts cert0 and port+1 trusts cert1.
 *      Cross-verifies v0.5.33's per-listener slot lookup reaches
 *      the actual handshake in production.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TLS_DIR "/tmp/cmq-test-v0545"

static void __attribute__((constructor)) install_sigpipe_handler(void) {
    signal(SIGPIPE, SIG_IGN);
}

static void ensure_dir(void) {
    int rc __attribute__((unused)) = system("rm -rf " TLS_DIR " && mkdir -p " TLS_DIR);
    (void)rc;
}

static void gen_cert(const char *cert, const char *key, const char *cn) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
        "-days 1 -nodes -subj '/CN=%s' 2>/dev/null", key, cert, cn);
    int rc __attribute__((unused)) = system(cmd);
    (void)rc;
}

static void *server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

/* Wait for the server's listen fds to be populated. Bounded poll. */
static void wait_for_bind(cmq_server_t *srv, int n_listeners) {
    for (int i = 0; i < 200; i++) {
        int ready = 1;
        for (int j = 0; j < n_listeners; j++) {
            if (srv->listen_fds[j] < 0) { ready = 0; break; }
        }
        if (ready) return;
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
}

static int open_tcp(uint16_t port) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(c, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(c);
        return -1;
    }
    return c;
}

/* Drive SSL_do_handshake on a dedicated thread, mirroring the
 * v0.5.28 pattern: each side of the handshake needs its own
 * select()/SSL_do_handshake loop, otherwise the OpenSSL state
 * machines deadlock on shared kernel buffer reads. */
struct hs_arg {
    SSL *ssl;
    int fd;
    int rc;
};
static void *hs_thread(void *a) {
    struct hs_arg *arg = a;
    fd_set rfds, wfds;
    for (int i = 0; i < 500; i++) {
        int r = SSL_do_handshake(arg->ssl);
        if (r == 1) { arg->rc = 1; return NULL; }
        int e = SSL_get_error(arg->ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            arg->rc = -1;
            return NULL;
        }
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        if (e == SSL_ERROR_WANT_READ) FD_SET(arg->fd, &rfds);
        if (e == SSL_ERROR_WANT_WRITE) FD_SET(arg->fd, &wfds);
        struct timeval tv = {0, 10000};
        select(arg->fd + 1, &rfds, &wfds, NULL, &tv);
    }
    arg->rc = -2;  /* timeout */
    return NULL;
}

/* Drive a full TLS handshake against `port` and trust `ca_cert`. */
static int drive_handshake(int fd, const char *ca_cert) {
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    if (!cctx) return -1;
    /* SSL_CTX_load_verify_file sets the trust store but does NOT
     * flip the verify mode — without SSL_VERIFY_PEER the client
     * accepts any cert chain. Enable peer verification explicitly
     * so the cross-check in test 2 actually exercises cert mismatch. */
    if (SSL_CTX_load_verify_file(cctx, ca_cert) != 1) {
        SSL_CTX_free(cctx);
        return -1;
    }
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
    SSL *cssl = SSL_new(cctx);
    if (!cssl) { SSL_CTX_free(cctx); return -1; }
    SSL_set_fd(cssl, fd);
    SSL_set_connect_state(cssl);

    struct hs_arg carg = { cssl, fd, 0 };
    pthread_t tid;
    if (pthread_create(&tid, NULL, hs_thread, &carg) != 0) {
        SSL_free(cssl); SSL_CTX_free(cctx);
        return -1;
    }
    pthread_join(tid, NULL);
    int rc = carg.rc;

    /* Drain the OpenSSL error stack so it doesn't leak to subsequent tests. */
    if (rc != 1) {
        unsigned long e;
        while ((e = ERR_get_error()))
            fprintf(stderr, "v0.5.45 handshake err: %s\n",
                    ERR_reason_error_string(e));
    }
    SSL_free(cssl);
    SSL_CTX_free(cctx);
    return rc;
}

/* ---------- Test 1: single listener ---------- */

TEST(tls_e2e_handshake, single_listener) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0545server");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25510;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/key.pem";
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    int cfd = open_tcp(25510);
    ASSERT(cfd >= 0);

    int rc = drive_handshake(cfd, TLS_DIR "/cert.pem");
    ASSERT_EQ(rc, 1);

    close(cfd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* ---------- Test 2: multi-listener distinct certs ---------- */

TEST(tls_e2e_handshake, multi_listener_distinct_certs) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert0.pem", TLS_DIR "/key0.pem", "v0545slot0");
    gen_cert(TLS_DIR "/cert1.pem", TLS_DIR "/key1.pem", "v0545slot1");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25511;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    /* Slot 0 (legacy fields). */
    cfg.tls_cert = TLS_DIR "/cert0.pem";
    cfg.tls_key = TLS_DIR "/key0.pem";
    /* Slot 1 (per-listener override). */
    cfg.listeners[1].tls_cert = TLS_DIR "/cert1.pem";
    cfg.listeners[1].tls_key = TLS_DIR "/key1.pem";
    cfg.listener_count = 2;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 2);
    ASSERT(srv->listen_fds[0] >= 0);
    ASSERT(srv->listen_fds[1] >= 0);
    ASSERT(srv->listen_fds[0] != srv->listen_fds[1]);

    /* Slot 0: port 25511, cert0. */
    int cfd0 = open_tcp(25511);
    ASSERT(cfd0 >= 0);
    int rc0 = drive_handshake(cfd0, TLS_DIR "/cert0.pem");
    ASSERT_EQ(rc0, 1);
    close(cfd0);

    /* Slot 1: port 25512 (= port+1), cert1. v0.5.33 must map this
     * listen fd to tls_config_slots[1] so the handshake uses cert1. */
    int cfd1 = open_tcp(25512);
    ASSERT(cfd1 >= 0);
    int rc1 = drive_handshake(cfd1, TLS_DIR "/cert1.pem");
    ASSERT_EQ(rc1, 1);
    close(cfd1);

    /* Cross-check: connect to slot 1's port but trust only cert0
     * → the server presents cert1, the client rejects it → handshake
     * fails. This confirms v0.5.33's slot lookup is real (a slot
     * collision would have presented cert0 here and made this pass). */
    int cfd_x = open_tcp(25512);
    ASSERT(cfd_x >= 0);
    int rc_x = drive_handshake(cfd_x, TLS_DIR "/cert0.pem");
    ASSERT(rc_x != 1);  /* must fail: server cert != trusted CA */
    close(cfd_x);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
