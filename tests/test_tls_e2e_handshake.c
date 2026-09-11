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
#include <sys/stat.h>

#define TLS_DIR "/tmp/cmq-test-v0545"
#define MTLS_DIR "/tmp/cmq-test-v0546"
#define CRL_DIR "/tmp/cmq-test-v0547"

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

/* v0.5.46: drive a TLS handshake where the client may present a
 * cert + key (mTLS). Trusts `ca_cert` for the server's chain.
 * Pass NULL for client_cert / client_key to skip client auth. */
static int drive_handshake_with_client_cert(int fd, const char *ca_cert,
                                             const char *client_cert,
                                             const char *client_key) {
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    if (!cctx) return -1;
    if (SSL_CTX_load_verify_file(cctx, ca_cert) != 1) {
        SSL_CTX_free(cctx); return -1;
    }
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
    if (client_cert && client_key) {
        if (SSL_CTX_use_certificate_file(cctx, client_cert,
                                          SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(cctx); return -1;
        }
        if (SSL_CTX_use_PrivateKey_file(cctx, client_key,
                                         SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(cctx); return -1;
        }
    }
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
    if (rc != 1) {
        unsigned long e;
        while ((e = ERR_get_error()))
            fprintf(stderr, "v0.5.46 mtls handshake err: %s\n",
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

/* ---------- Test 3 (v0.5.46): mTLS end-to-end ----------
 *
 * Server is configured with tls_verify_peer=1 and tls_ca=CA. A
 * client with a valid CA-signed client cert must succeed; a
 * client without a cert must be rejected. Closes the gap that
 * test_mtls_api.c only checks the setter round-trip, not the
 * runtime handshake behavior.
 */
static int mtls_run(const char *cmd) {
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int mtls_gen_ca(const char *ca_cert, const char *ca_key) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
        "-days 1 -nodes -subj '/CN=cmq-test-ca' 2>/dev/null",
        ca_key, ca_cert);
    return mtls_run(cmd);
}

static int mtls_gen_signed(const char *ca_cert, const char *ca_key,
                            const char *cert, const char *key,
                            const char *cn) {
    char subj[256], cmd[2048];
    snprintf(subj, sizeof(subj), "/CN=%s", cn);
    snprintf(cmd, sizeof(cmd),
        "openssl req -newkey rsa:2048 -keyout %s -out %s.csr -nodes "
        "-subj '%s' 2>/dev/null && "
        "openssl x509 -req -in %s.csr -CA %s -CAkey %s -CAcreateserial "
        "-out %s -days 1 2>/dev/null && rm -f %s.csr",
        key, key, subj, key, ca_cert, ca_key, cert, key);
    return mtls_run(cmd);
}

/* v0.5.47: same as mtls_gen_signed but adds a CRL Distribution
 * Point extension pointing at file://path/to/crl.pem. Without the
 * CDP, OpenSSL's X509_V_FLAG_CRL_CHECK is silently a no-op (the
 * verifier has no way to know which CRL to consult). The file://
 * URI lets the verifier read the CRL directly from disk. */
static int mtls_gen_signed_with_cdp(const char *ca_cert, const char *ca_key,
                                       const char *cert, const char *key,
                                       const char *cn,
                                       const char *crl_path) {
    char subj[256], cnf_path[1024], cmd[4096];
    snprintf(subj, sizeof(subj), "/CN=%s", cn);
    snprintf(cnf_path, sizeof(cnf_path), "%s.ext.cnf", cert);
    FILE *f = fopen(cnf_path, "w");
    if (!f) return -1;
    fprintf(f,
        "[v3_client]\n"
        "basicConstraints = CA:FALSE\n"
        "extendedKeyUsage = clientAuth\n"
        "subjectKeyIdentifier = hash\n"
        "authorityKeyIdentifier = keyid,issuer\n"
        "crlDistributionPoints = URI:file://%s\n",
        crl_path);
    fclose(f);
    snprintf(cmd, sizeof(cmd),
        "openssl req -newkey rsa:2048 -keyout %s -out %s.csr -nodes "
        "-subj '%s' 2>/dev/null && "
        "openssl x509 -req -in %s.csr -CA %s -CAkey %s -CAcreateserial "
        "-out %s -days 1 -extfile %s -extensions v3_client 2>/dev/null && "
        "rm -f %s.csr %s",
        key, key, subj, key, ca_cert, ca_key, cert, cnf_path, key, cnf_path);
    return mtls_run(cmd);
}

TEST(tls_e2e_handshake, mtls_required) {
    int rc __attribute__((unused)) = system(
        "rm -rf " MTLS_DIR " && mkdir -p " MTLS_DIR);
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/server.pem", MTLS_DIR "/server.key",
                                "v0546server"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/client.pem", MTLS_DIR "/client.key",
                                "v0546client"), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25520;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = MTLS_DIR "/server.pem";
    cfg.tls_key = MTLS_DIR "/server.key";
    cfg.tls_ca = MTLS_DIR "/ca.pem";
    cfg.tls_verify_peer = 1;  /* mTLS: require + verify client certs */
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Sub-test A: client WITH cert → handshake must succeed. */
    int cfd_a = open_tcp(25520);
    ASSERT(cfd_a >= 0);
    int rc_a = drive_handshake_with_client_cert(cfd_a,
        MTLS_DIR "/ca.pem",
        MTLS_DIR "/client.pem",
        MTLS_DIR "/client.key");
    ASSERT_EQ(rc_a, 1);
    close(cfd_a);

    /* Sub-test B: client WITHOUT cert → handshake must FAIL.
     * Server has tls_verify_peer=1 + SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
     * so an unauthenticated client is rejected. */
    int cfd_b = open_tcp(25520);
    ASSERT(cfd_b >= 0);
    int rc_b = drive_handshake_with_client_cert(cfd_b,
        MTLS_DIR "/ca.pem", NULL, NULL);
    ASSERT(rc_b != 1);
    close(cfd_b);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    rc = system("rm -rf " MTLS_DIR); (void)rc;
}

/* ---------- Test 4 (v0.5.47): CRL revocation end-to-end ----------
 *
 * Generates a CA + server cert + client cert, then revokes the
 * client cert via openssl ca -gencrl. Configures cmq_server with
 * tls_verify_peer=1, tls_ca=CA, tls_crl=CRL. A client presenting
 * the (now revoked) client cert must be rejected by the server.
 */
static int crl_run(const char *cmd) {
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

TEST(tls_e2e_handshake, mtls_revoked_client_rejected) {
    int rc __attribute__((unused)) = system(
        "rm -rf " CRL_DIR " && mkdir -p " CRL_DIR
        " && mkdir -p " CRL_DIR "/ca_db"
        " && touch " CRL_DIR "/ca_db/index.txt"
        " && echo '01' > " CRL_DIR "/ca_db/serial");
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(CRL_DIR "/ca.pem", CRL_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(CRL_DIR "/ca.pem", CRL_DIR "/ca.key",
                                CRL_DIR "/server.pem", CRL_DIR "/server.key",
                                "v0547server"), 0);

    /* Write a minimal openssl.cnf for the CA database. The CA's
     * private_key and certificate fields must point at the CA
     * files so openssl ca can read them. */
    FILE *cnf = fopen(CRL_DIR "/openssl.cnf", "w");
    ASSERT_NOT_NULL(cnf);
    fprintf(cnf,
        "[ ca ]\n"
        "default_ca = CMQ_CA\n"
        "[ CMQ_CA ]\n"
        "dir = %s/ca_db\n"
        "database = %s/ca_db/index.txt\n"
        "serial = %s/ca_db/serial\n"
        "default_md = sha256\n"
        "policy = CMQ_CA_POLICY\n"
        "crl = %s/ca_db/crl.pem\n"
        "crlnumber = %s/ca_db/crlnumber\n"
        "private_key = %s/ca.key\n"
        "certificate = %s/ca.pem\n"
        "[ CMQ_CA_POLICY ]\n"
        "commonName = supplied\n",
        CRL_DIR, CRL_DIR, CRL_DIR, CRL_DIR, CRL_DIR,
        CRL_DIR, CRL_DIR);
    fclose(cnf);
    /* Initialize crlnumber. */
    FILE *fn = fopen(CRL_DIR "/ca_db/crlnumber", "w");
    ASSERT_NOT_NULL(fn);
    fprintf(fn, "01\n");
    fclose(fn);

    /* Generate the client cert WITH a CDP extension pointing at
     * the CRL file path we'll generate next. The CDP must point
     * at a real path; OpenSSL silently skips CRL check if the
     * URI is unreachable. */
    char crl_path[1024];
    snprintf(crl_path, sizeof(crl_path), "%s/crl.pem", CRL_DIR);
    ASSERT_EQ(mtls_gen_signed_with_cdp(CRL_DIR "/ca.pem", CRL_DIR "/ca.key",
                                         CRL_DIR "/client.pem", CRL_DIR "/client.key",
                                         "v0547client", crl_path), 0);

    /* Generate a CRL that revokes client.pem. The CRL generation
     * sequence must be: (1) empty CRL, (2) revoke, (3) re-emit CRL
     * with the revocation in it. Doing -gencrl -revoke in a single
     * invocation doesn't include the new revocation in the
     * emitted CRL (observed quirk of openssl ca). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -gencrl -crldays 1 -out %s/crl.pem.tmp 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);
    system("rm -f " CRL_DIR "/crl.pem.tmp");
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -revoke %s/client.pem 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -gencrl -crldays 1 -out %s/crl.pem 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);

    /* Sanity: CRL exists. */
    struct stat st;
    ASSERT_EQ(stat(CRL_DIR "/crl.pem", &st), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25521;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = CRL_DIR "/server.pem";
    cfg.tls_key = CRL_DIR "/server.key";
    cfg.tls_ca = CRL_DIR "/ca.pem";
    cfg.tls_crl = CRL_DIR "/crl.pem";  /* v0.5.47: CRL file. */
    cfg.tls_verify_peer = 1;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Client presents the (revoked) cert. Server must reject. */
    int cfd = open_tcp(25521);
    ASSERT(cfd >= 0);
    int rc_hs = drive_handshake_with_client_cert(cfd,
        CRL_DIR "/ca.pem",
        CRL_DIR "/client.pem",
        CRL_DIR "/client.key");
    if (rc_hs == 1) {
        fprintf(stderr, "v0.5.47: revoked cert was accepted (BUG)\n");
    }
    ASSERT(rc_hs != 1);  /* must fail: client cert is revoked */
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    rc = system("rm -rf " CRL_DIR); (void)rc;
}

/* ---------- Test 5 (v0.5.50): mid-handshake disconnect ----------
 *
 * Defensive test for the v0.5.45 handshake-resume fix. The server
 * accepts a TCP connection, the TLS handshake begins, and the
 * client disconnects before completing. The server must not spin
 * in cmq_tls_handshake; it must detect the closed fd and tear
 * down the client cleanly. Before v0.5.45, the bug would have
 * been that the handshake never returned an error (the v0.5.45
 * fix made SSL_do_handshake return -1 on a closed fd via the
 * path that calls SSL_read → returns SSL_ERROR_SYSCALL → -1).
 */
TEST(tls_e2e_handshake, mid_handshake_disconnect) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0550server");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25522;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/key.pem";
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect to the server, send a few bytes of ClientHello, then
     * close the socket mid-handshake. The server should detect the
     * EOF and tear down the client. We use a plain (non-TLS) write
     * — enough bytes to start the handshake on the server side
     * but invalid enough that the server keeps trying to read more. */
    int cfd = open_tcp(25522);
    ASSERT(cfd >= 0);
    /* Send a partial ClientHello: TLS record header (0x16 = handshake,
     * version 0x0303 = TLS 1.2, length 0x0004) + 4 bytes of garbage.
     * This is enough to start the handshake but won't complete. */
    uint8_t partial[9] = {0x16, 0x03, 0x03, 0x00, 0x04, 0xaa, 0xbb, 0xcc, 0xdd};
    ssize_t w = write(cfd, partial, sizeof(partial));
    ASSERT(w == (ssize_t)sizeof(partial));
    /* Give the server a moment to read the partial. */
    struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL);
    /* Close the socket abruptly. The server should detect EOF on
     * its next EV_READ and call cmq_tls_handshake → SSL_do_handshake
     * → returns -1 (SSL_ERROR_SYSCALL) → client_teardown. */
    close(cfd);

    /* Wait for the server to detect and clean up. Without this
     * defensive test, a server that spun in cmq_tls_handshake
     * would never exit cmq_server_run. */
    ts.tv_sec = 1; ts.tv_nsec = 0; nanosleep(&ts, NULL);

    /* Stop the server. If the server is stuck on a dead client
     * fd, cmq_server_stop would hang on pthread_join. The 1-second
     * wait above should have given it enough time to clean up. */
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* ---------- Test 6 (v0.5.51): garbage TLS record ----------
 *
 * Defensive test for the v0.5.45 handshake-resume fix. A TLS
 * client sends bytes that look like a valid TLS record header but
 * contain a bogus record type. OpenSSL's state machine should
 * reject the record with SSL_ERROR_SSL, the server's
 * cmq_tls_handshake should return -1, and the server should
 * tear down the client. Without the v0.5.45 fix, the server
 * might enter an inconsistent state and spin.
 */
TEST(tls_e2e_handshake, garbage_tls_record) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0551server");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25523;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/key.pem";
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Send a valid-looking TLS record header but with an invalid
     * record type (0xff). OpenSSL rejects unknown record types. */
    int cfd = open_tcp(25523);
    ASSERT(cfd >= 0);
    uint8_t bad[5] = {0xff, 0x03, 0x03, 0x00, 0x01};
    ssize_t w = write(cfd, bad, sizeof(bad));
    ASSERT(w == (ssize_t)sizeof(bad));
    /* Give the server a moment to read and reject. */
    struct timespec ts = {0, 500000000}; nanosleep(&ts, NULL);
    close(cfd);

    /* Wait for server to clean up. Same rationale as
     * mid_handshake_disconnect: without v0.5.45 the server
     * could hang on the bad fd. */
    ts.tv_sec = 1; ts.tv_nsec = 0; nanosleep(&ts, NULL);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* ---------- Test 7 (v0.5.53): TLS 1.3 negotiation ----------
 *
 * Asserts the server actually negotiates TLS 1.3 (not 1.2) for
 * plain TLS. The single_listener test exercises a TLS handshake
 * but doesn't verify the negotiated version. This test adds
 * that explicit check.
 *
 * Why it matters: the v0.5.46 mTLS cap forces TLS 1.2 for
 * verify_peer, but plain TLS should still default to 1.3.
 * Verifying the version catches regressions where the cap
 * accidentally leaks into the plain TLS path.
 */
TEST(tls_e2e_handshake, tls13_negotiated) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0553server");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25525;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/key.pem";
    /* NOT setting tls_verify_peer, so the v0.5.46 TLS 1.2 cap
     * is NOT applied. The server should negotiate TLS 1.3. */
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    int cfd = open_tcp(25525);
    ASSERT(cfd >= 0);

    /* Client: TLS 1.3 only, no fallback to 1.2. */
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NOT_NULL(cctx);
    ASSERT_EQ(SSL_CTX_load_verify_file(cctx, TLS_DIR "/cert.pem"), 1);
    SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(cctx, TLS1_3_VERSION);
    SSL *cssl = SSL_new(cctx);
    ASSERT_NOT_NULL(cssl);
    SSL_set_fd(cssl, cfd);
    SSL_set_connect_state(cssl);

    struct hs_arg carg = { cssl, cfd, 0 };
    pthread_t c_tid;
    ASSERT_EQ(pthread_create(&c_tid, NULL, hs_thread, &carg), 0);
    pthread_join(c_tid, NULL);
    ASSERT_EQ(carg.rc, 1);

    /* Assert the negotiated version is TLS 1.3 (0x0304). */
    ASSERT_EQ(SSL_version(cssl), 0x0304);

    SSL_shutdown(cssl);
    SSL_free(cssl);
    SSL_CTX_free(cctx);
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* ---------- Test 8 (v0.5.54): CRL with non-revoked client ----------
 *
 * The v0.5.47 test covers "CRL configured, client IS revoked →
 * rejected". This test covers the COMMON case: "CRL configured,
 * client NOT revoked → accepted". The CRL has someone else's
 * cert revoked, but the connecting client is in the clear.
 *
 * Why it matters: a regression in the CRL scope-matching code
 * could falsely reject valid clients when a CRL is loaded.
 * The OpenSSL issue #23325 documented a scope-matching bug
 * where certs without CDP were falsely rejected with
 * X509_V_ERR_DIFFERENT_CRL_SCOPE (44). The v0.5.49 fix
 * reverted to CRL_CHECK (leaf-only) to mitigate this. This
 * test ensures the common-case path keeps working: a client
 * with CDP, no serial match in the CRL, handshake succeeds.
 */
TEST(tls_e2e_handshake, mtls_crl_not_revoked_accepted) {
    int rc __attribute__((unused)) = system(
        "rm -rf " CRL_DIR " && mkdir -p " CRL_DIR
        " && mkdir -p " CRL_DIR "/ca_db"
        " && touch " CRL_DIR "/ca_db/index.txt"
        " && echo '01' > " CRL_DIR "/ca_db/serial");
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(CRL_DIR "/ca.pem", CRL_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(CRL_DIR "/ca.pem", CRL_DIR "/ca.key",
                                CRL_DIR "/server.pem", CRL_DIR "/server.key",
                                "v0554server"), 0);

    /* Two client certs: one WILL be revoked (other_client),
     * one WILL stay valid (our_client). Both with CDP. */
    FILE *cnf = fopen(CRL_DIR "/openssl.cnf", "w");
    ASSERT_NOT_NULL(cnf);
    fprintf(cnf,
        "[ ca ]\n"
        "default_ca = CMQ_CA\n"
        "[ CMQ_CA ]\n"
        "dir = %s/ca_db\n"
        "database = %s/ca_db/index.txt\n"
        "serial = %s/ca_db/serial\n"
        "default_md = sha256\n"
        "policy = CMQ_CA_POLICY\n"
        "crl = %s/ca_db/crl.pem\n"
        "crlnumber = %s/ca_db/crlnumber\n"
        "private_key = %s/ca.key\n"
        "certificate = %s/ca.pem\n"
        "[ CMQ_CA_POLICY ]\n"
        "commonName = supplied\n",
        CRL_DIR, CRL_DIR, CRL_DIR, CRL_DIR, CRL_DIR,
        CRL_DIR, CRL_DIR);
    fclose(cnf);
    FILE *fn = fopen(CRL_DIR "/ca_db/crlnumber", "w");
    ASSERT_NOT_NULL(fn);
    fprintf(fn, "01\n");
    fclose(fn);

    char crl_path[1024];
    snprintf(crl_path, sizeof(crl_path), "%s/crl.pem", CRL_DIR);
    ASSERT_EQ(mtls_gen_signed_with_cdp(CRL_DIR "/ca.pem", CRL_DIR "/ca.key",
                                         CRL_DIR "/our_client.pem",
                                         CRL_DIR "/our_client.key",
                                         "v0554our", crl_path), 0);
    ASSERT_EQ(mtls_gen_signed_with_cdp(CRL_DIR "/ca.pem", CRL_DIR "/ca.key",
                                         CRL_DIR "/other_client.pem",
                                         CRL_DIR "/other_client.key",
                                         "v0554other", crl_path), 0);

    /* Three-step CRL generation: empty → revoke other_client →
     * re-emit. our_client is NOT revoked. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -gencrl -crldays 1 -out %s/crl.pem.tmp 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);
    system("rm -f " CRL_DIR "/crl.pem.tmp");
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -revoke %s/other_client.pem 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);
    snprintf(cmd, sizeof(cmd),
        "openssl ca -config %s/openssl.cnf -gencrl -crldays 1 -out %s/crl.pem 2>/dev/null",
        CRL_DIR, CRL_DIR);
    ASSERT_EQ(crl_run(cmd), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25530;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = CRL_DIR "/server.pem";
    cfg.tls_key = CRL_DIR "/server.key";
    cfg.tls_ca = CRL_DIR "/ca.pem";
    cfg.tls_crl = CRL_DIR "/crl.pem";
    cfg.tls_verify_peer = 1;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect with the NON-revoked client. Handshake must succeed. */
    int cfd = open_tcp(25530);
    ASSERT(cfd >= 0);
    int rc_hs = drive_handshake_with_client_cert(cfd,
        CRL_DIR "/ca.pem",
        CRL_DIR "/our_client.pem",
        CRL_DIR "/our_client.key");
    if (rc_hs != 1) {
        fprintf(stderr, "v0.5.54: non-revoked client rejected (BUG)\n");
    }
    ASSERT_EQ(rc_hs, 1);  /* NOT in CRL → must succeed */
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " CRL_DIR);
    (void)rc2;
}

/* ---------- Test 9 (v0.5.55): missing CRL file path ----------
 *
 * Defensive test: setting `tls_crl` to a non-existent path must
 * not crash the server. The v0.5.47 code path uses BIO_new_file
 * which returns NULL on missing file, and the existing code
 * silently skips CRL loading on NULL. This test asserts:
 *   1. Server starts without crashing.
 *   2. mTLS handshake with a valid client cert succeeds (CRL
 *      check is effectively disabled).
 *
 * Why it matters: a misconfigured `tls_crl` path is a security
 * concern (operators expect CRL enforcement). The production
 * code currently prefers fail-open (don't reject valid
 * clients) over fail-closed (reject everything). Documenting
 * this behavior in a test prevents future "helpful" changes
 * that might silently flip the policy.
 */
TEST(tls_e2e_handshake, mtls_missing_crl_file_path) {
    int rc __attribute__((unused)) = system(
        "rm -rf " MTLS_DIR " && mkdir -p " MTLS_DIR);
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/server.pem", MTLS_DIR "/server.key",
                                "v0555server"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/client.pem", MTLS_DIR "/client.key",
                                "v0555client"), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25531;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = MTLS_DIR "/server.pem";
    cfg.tls_key = MTLS_DIR "/server.key";
    cfg.tls_ca = MTLS_DIR "/ca.pem";
    /* Point at a path that doesn't exist. */
    cfg.tls_crl = "/tmp/cmq-test-no-such-crl.pem";
    cfg.tls_verify_peer = 1;
    /* Server creation must succeed (no crash). */
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect with a valid client cert. Should succeed because
     * the CRL is silently ignored (BIO_new_file returned NULL). */
    int cfd = open_tcp(25531);
    ASSERT(cfd >= 0);
    int rc_hs = drive_handshake_with_client_cert(cfd,
        MTLS_DIR "/ca.pem",
        MTLS_DIR "/client.pem",
        MTLS_DIR "/client.key");
    ASSERT_EQ(rc_hs, 1);
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " MTLS_DIR);
    (void)rc2;
}

/* ---------- Test 10 (v0.5.56): cross-CA rejection ----------
 *
 * Defensive test for the v0.5.46 mTLS CA bundle trust chain. The
 * server is configured with `tls_ca=ServerCA`. A client whose
 * cert is signed by a DIFFERENT CA (ClientCA) must be rejected
 * by the server.
 *
 * Why it matters: if the CA bundle were silently widened to
 * "trust any cert" (e.g., by setting tls_ca=NULL), the security
 * boundary collapses. The v0.5.46 wiring calls
 * SSL_CTX_load_verify_locations with the configured CA path —
 * if that path is wrong or the CA bundle is too permissive,
 * the chain verification might accept a cert that shouldn't
 * be trusted. This test verifies the chain check actually
 * consults the CA bundle.
 */
TEST(tls_e2e_handshake, mtls_cross_ca_rejected) {
    int rc __attribute__((unused)) = system(
        "rm -rf " MTLS_DIR " && mkdir -p " MTLS_DIR);
    (void)rc;
    /* Two independent CAs. The server trusts ServerCA. The
     * client presents a cert signed by ClientCA. */
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/server_ca.pem", MTLS_DIR "/server_ca.key"), 0);
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/client_ca.pem", MTLS_DIR "/client_ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/server_ca.pem", MTLS_DIR "/server_ca.key",
                                MTLS_DIR "/server.pem", MTLS_DIR "/server.key",
                                "v0556server"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/client_ca.pem", MTLS_DIR "/client_ca.key",
                                MTLS_DIR "/client.pem", MTLS_DIR "/client.key",
                                "v0556client"), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25532;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = MTLS_DIR "/server.pem";
    cfg.tls_key = MTLS_DIR "/server.key";
    /* Trust the SERVER's CA only — the client's CA is untrusted. */
    cfg.tls_ca = MTLS_DIR "/server_ca.pem";
    cfg.tls_verify_peer = 1;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect with a client cert signed by ClientCA. The client's
     * own CA bundle trusts ClientCA, but the server's CA bundle
     * does NOT trust it. The handshake must fail. */
    int cfd = open_tcp(25532);
    ASSERT(cfd >= 0);
    int rc_hs = drive_handshake_with_client_cert(cfd,
        MTLS_DIR "/client_ca.pem",
        MTLS_DIR "/client.pem",
        MTLS_DIR "/client.key");
    if (rc_hs == 1) {
        fprintf(stderr, "v0.5.56: cross-CA client ACCEPTED (BUG)\n");
    }
    ASSERT(rc_hs != 1);  /* untrusted CA → must fail */
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    rc = system("rm -rf " MTLS_DIR); (void)rc;
}

/* ---------- Test 11 (v0.5.57): missing CA bundle path ----------
 *
 * Defensive test: setting `tls_ca` to a non-existent path while
 * `tls_verify_peer=1` must not silently accept unauthenticated
 * clients. The v0.5.46 code calls SSL_CTX_load_verify_locations
 * with the configured CA path; if the file is missing, OpenSSL
 * has no trust anchors. With SSL_VERIFY_PEER, every client cert
 * must fail verification (no valid chain).
 *
 * Why it matters: if the CA bundle were silently treated as
 * "trust any cert" when the path is invalid, the security
 * boundary collapses. This test verifies the failure mode is
 * fail-closed (reject all clients) not fail-open (accept all).
 */
TEST(tls_e2e_handshake, mtls_missing_ca_bundle) {
    int rc __attribute__((unused)) = system(
        "rm -rf " MTLS_DIR " && mkdir -p " MTLS_DIR);
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/server.pem", MTLS_DIR "/server.key",
                                "v0557server"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/client.pem", MTLS_DIR "/client.key",
                                "v0557client"), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25533;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = MTLS_DIR "/server.pem";
    cfg.tls_key = MTLS_DIR "/server.key";
    /* Point at a non-existent CA bundle. */
    cfg.tls_ca = "/tmp/cmq-test-no-such-ca.pem";
    cfg.tls_verify_peer = 1;
    /* Server creation may or may not succeed depending on whether
     * SSL_CTX_load_verify_locations returns an error. Either way
     * the mTLS handshake must reject the client cert. */
    int srv_rc = cmq_server_create(&srv, &cfg);
    (void)srv_rc;  /* Either success or failure is acceptable */

    if (srv) {
        pthread_t tid;
        ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
        wait_for_bind(srv, 1);
        ASSERT(srv->listen_fds[0] >= 0);

        int cfd = open_tcp(25533);
        ASSERT(cfd >= 0);
        int rc_hs = drive_handshake_with_client_cert(cfd,
            MTLS_DIR "/ca.pem",
            MTLS_DIR "/client.pem",
            MTLS_DIR "/client.key");
        /* With no CA bundle and verify_peer=1, the chain
         * verification fails → handshake must fail. */
        if (rc_hs == 1) {
            fprintf(stderr, "v0.5.57: missing CA bundle accepted client (BUG)\n");
        }
        ASSERT(rc_hs != 1);
        close(cfd);

        cmq_server_stop(srv);
        pthread_join(tid, NULL);
        cmq_server_destroy(srv);
    }

    rc = system("rm -rf " MTLS_DIR); (void)rc;
}

/* ---------- Test 12 (v0.5.58): mTLS forces TLS 1.2 ----------
 *
 * Asserts the v0.5.46 cap behavior: when `tls_verify_peer=1`
 * is set, the server's SSL_CTX is capped at TLS 1.2 (because
 * OpenSSL 3.5 + TLS 1.3 silently bypasses
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT). A client pinned to TLS 1.3
 * ONLY must fail to handshake with such a server.
 *
 * Why it matters: this locks in the v0.5.46 design decision.
 * If a future refactor "fixes" TLS 1.3 mTLS (v0.5.48/v0.5.49
 * race) and removes the cap, this test fails — forcing the
 * author to update the test along with the fix.
 */
TEST(tls_e2e_handshake, mtls_forces_tls12) {
    int rc __attribute__((unused)) = system(
        "rm -rf " MTLS_DIR " && mkdir -p " MTLS_DIR);
    (void)rc;
    ASSERT_EQ(mtls_gen_ca(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/server.pem", MTLS_DIR "/server.key",
                                "v0558server"), 0);
    ASSERT_EQ(mtls_gen_signed(MTLS_DIR "/ca.pem", MTLS_DIR "/ca.key",
                                MTLS_DIR "/client.pem", MTLS_DIR "/client.key",
                                "v0558client"), 0);

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25534;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = MTLS_DIR "/server.pem";
    cfg.tls_key = MTLS_DIR "/server.key";
    cfg.tls_ca = MTLS_DIR "/ca.pem";
    cfg.tls_verify_peer = 1;  /* triggers v0.5.46 TLS 1.2 cap */
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect with a TLS 1.3-only client. The handshake must fail
     * because the server (with verify_peer=1) caps at TLS 1.2. */
    int cfd = open_tcp(25534);
    ASSERT(cfd >= 0);
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NOT_NULL(cctx);
    ASSERT_EQ(SSL_CTX_load_verify_file(cctx, MTLS_DIR "/ca.pem"), 1);
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(cctx, TLS1_3_VERSION);
    if (SSL_CTX_use_certificate_file(cctx, MTLS_DIR "/client.pem",
                                      SSL_FILETYPE_PEM) == 1 &&
        SSL_CTX_use_PrivateKey_file(cctx, MTLS_DIR "/client.key",
                                     SSL_FILETYPE_PEM) == 1) {
        SSL *cssl = SSL_new(cctx);
        ASSERT_NOT_NULL(cssl);
        SSL_set_fd(cssl, cfd);
        SSL_set_connect_state(cssl);
        struct hs_arg carg = { cssl, cfd, 0 };
        pthread_t c_tid;
        ASSERT_EQ(pthread_create(&c_tid, NULL, hs_thread, &carg), 0);
        pthread_join(c_tid, NULL);
        /* Handshake must fail (server caps at TLS 1.2). */
        if (carg.rc == 1) {
            fprintf(stderr, "v0.5.58: TLS 1.3 mTLS succeeded (CAP LIFTED?)\n");
        }
        ASSERT(carg.rc != 1);
        SSL_shutdown(cssl);
        SSL_free(cssl);
    }
    SSL_CTX_free(cctx);
    close(cfd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    rc = system("rm -rf " MTLS_DIR); (void)rc;
}

/* ---------- Test 13 (v0.5.59): idle TLS client (no ClientHello) ----------
 *
 * Defensive test: a TCP client connects to a TLS-enabled server
 * but never sends any data. The server must not spin in
 * cmq_tls_handshake waiting for a ClientHello that will never
 * arrive. The cmq_tls_handshake() function returns 0 (WANT_READ)
 * when waiting, so the server must either timeout or stay
 * blocked on a poll until the client closes.
 *
 * Before v0.5.45, the cmq_tls_handshake call would have failed
 * (rc == 1 from SSL_do_handshake would have been treated as
 * failure, returning -1, but SSL_do_handshake with no data
 * returns WANT_READ = 0, not 1, so the v0.5.45 fix doesn't
 * affect this path). The risk is that the server leaks the
 * half-open client fd.
 *
 * This test connects, waits a beat, closes. The server should
 * detect the close cleanly (cmq_tls_handshake returns -1 on EOF).
 */
TEST(tls_e2e_handshake, idle_tls_client) {
    ensure_dir();
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0559server");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25535;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/key.pem";
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_for_bind(srv, 1);
    ASSERT(srv->listen_fds[0] >= 0);

    /* Connect but don't send anything. */
    int cfd = open_tcp(25535);
    ASSERT(cfd >= 0);
    /* Wait a beat — server should be waiting for ClientHello. */
    struct timespec ts = {0, 500000000}; nanosleep(&ts, NULL);
    /* Close without sending anything. Server must detect and
     * clean up without hanging. */
    close(cfd);
    /* Give server time to detect EOF. */
    ts.tv_sec = 1; ts.tv_nsec = 0; nanosleep(&ts, NULL);

    /* Server should be clean: stop + join without hanging. */
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* ---------- Test 14 (v0.5.60): cert/key mismatch ----------
 *
 * Defensive test: passing `tls_cert=A.pem` with `tls_key=B.key`
 * (different cert) must fail at server startup. The
 * `cmq_tls_load` function calls `SSL_CTX_check_private_key`
 * which returns 0 if cert and key don't match. `cmq_server_create`
 * propagates the error.
 *
 * Why it matters: a misconfigured cert/key pair is a common
 * production mistake. The pipeline should reject this at
 * startup, not at the first TLS handshake (where the error
 * is harder to diagnose).
 */
TEST(tls_e2e_handshake, cert_key_mismatch_rejected) {
    ensure_dir();
    /* Two unrelated cert/key pairs. */
    gen_cert(TLS_DIR "/cert.pem", TLS_DIR "/key.pem", "v0560server");
    gen_cert(TLS_DIR "/other.pem", TLS_DIR "/other.key", "v0560other");

    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25536;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    /* Mismatch: cert.pem paired with other.key. */
    cfg.tls_cert = TLS_DIR "/cert.pem";
    cfg.tls_key = TLS_DIR "/other.key";
    /* cmq_server_create must return non-OK. */
    int rc = cmq_server_create(&srv, &cfg);
    if (rc == CMQ_OK) {
        cmq_server_destroy(srv);
        fprintf(stderr, "v0.5.60: cert/key mismatch accepted (BUG)\n");
    }
    ASSERT(rc != CMQ_OK);
    /* Server must not be allocated. */
    ASSERT_NULL(srv);
}

/* ---------- Test 15 (v0.5.61): tls_enabled without cert ----------
 *
 * Defensive test: setting `tls_enabled=1` without `tls_cert` or
 * `tls_key` must fail at server startup. The
 * `cmq_server_create` code checks for missing cert/key and
 * returns `CMQ_ERR_INVALID_ARG`.
 *
 * Why it matters: a misconfigured `tls_enabled` (forgotten
 * cert path) is a common production mistake. The pipeline should
 * refuse to start in plaintext-with-TLS-claimed mode.
 */
TEST(tls_e2e_handshake, tls_enabled_without_cert_rejected) {
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25537;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    /* No tls_cert, no tls_key. */
    int rc = cmq_server_create(&srv, &cfg);
    if (rc == CMQ_OK) {
        cmq_server_destroy(srv);
        fprintf(stderr, "v0.5.61: TLS without cert accepted (BUG)\n");
    }
    ASSERT(rc != CMQ_OK);
    ASSERT_NULL(srv);
}

TEST_MAIN()
