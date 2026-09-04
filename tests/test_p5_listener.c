/* P5: per-listener SSL_CTX slots.
 * Verifies cmq_server_t can hold at least 2 distinct cmq_tls_config_t
 * slots (even though only slot[0] is wired in v0.5.1). */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_tls.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define P5_TEST_DIR "/tmp/cmq-test-p5-listener"

static void ensure_dir(void) {
    int rc __attribute__((unused)) = system("rm -rf " P5_TEST_DIR " && mkdir -p " P5_TEST_DIR);
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

TEST(p5, server_holds_distinct_tls_slots) {
    /* This test verifies the server struct shape: it must expose
     * tls_config_slots[] array with at least CMQ_MAX_LISTENERS slots.
     * We don't drive a full server here — that's covered by the
     * integration tests. Just check the field exists. */
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19988;
    cfg.log_to_stdout = 0;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    /* Without TLS configured, count is 0, slots are NULL. */
    ASSERT_EQ(srv->tls_config_count, 0);
    ASSERT_NULL(srv->tls_config_slots[0]);
    cmq_server_destroy(srv);
}

TEST(p5, multi_listener_data_structure) {
    /* P2 v0.5.9: cmq_config_t.listeners[4] + listener_count.
     * The runtime accept loop iterates only slot[0] (multi-listener
     * runtime is v0.6). v0.5.9 verifies the data structure. */
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19987;
    cfg.log_to_stdout = 0;
    cfg.listener_count = 0;
    cfg.listeners[0].tls_cert = NULL;
    cfg.listeners[0].tls_key = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    cmq_server_destroy(srv);
}

TEST(p5, tls_enabled_populates_slot0) {
    ensure_dir();
    gen_cert(P5_TEST_DIR "/cert.pem", P5_TEST_DIR "/key.pem", "p5server");
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19987;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = P5_TEST_DIR "/cert.pem";
    cfg.tls_key = P5_TEST_DIR "/key.pem";
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    ASSERT_EQ(srv->tls_config_count, 1);
    ASSERT_NOT_NULL(srv->tls_config_slots[0]);
    cmq_server_destroy(srv);
    int rc __attribute__((unused)) = system("rm -rf " P5_TEST_DIR);
    (void)rc;
}

TEST(p5, listeners_array_present) {
    /* P2 (v0.5.2): cmq_config_t exposes a listeners[4] array.
     * listener_count defaults to 0; legacy tls_cert path still works
     * (covered by tls_enabled_populates_slot0 above). */
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19986;
    cfg.log_to_stdout = 0;
    /* Manually set listener_count to exercise the array. */
    cfg.listeners[0].tls_cert = NULL;
    cfg.listeners[0].tls_key = NULL;
    cfg.listener_count = 1;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    cmq_server_destroy(srv);
}

/* v0.5.31: per-listener TLS runtime. Configures two distinct certs
 * on listeners[0] and listeners[1]; verifies that cmq_server_create
 * allocates both tls_config_slots[0] and tls_config_slots[1].
 * The v0.5.30 cache isolation test already proved the per-slot cache
 * is independent; this test confirms the production path populates
 * both slots. */
TEST(p5, per_listener_tls_two_certs) {
    ensure_dir();
    gen_cert(P5_TEST_DIR "/cert0.pem", P5_TEST_DIR "/key0.pem", "p5server0");
    gen_cert(P5_TEST_DIR "/cert1.pem", P5_TEST_DIR "/key1.pem", "p5server1");
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    /* Use a port outside the test_server port-guarded range (28800-28999)
     * so the multi-listener runtime activates and binds port+1. */
    cfg.port = 25001;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    /* Slot 0: legacy fields, used as the back-compat path. */
    cfg.tls_cert = P5_TEST_DIR "/cert0.pem";
    cfg.tls_key = P5_TEST_DIR "/key0.pem";
    /* Slot 1: per-listener override. */
    cfg.listeners[1].tls_cert = P5_TEST_DIR "/cert1.pem";
    cfg.listeners[1].tls_key = P5_TEST_DIR "/key1.pem";
    /* listener_count must be >= 2 for the multi-listener bind. */
    cfg.listener_count = 2;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Both slots populated, count = 2. */
    ASSERT_NOT_NULL(srv->tls_config_slots[0]);
    ASSERT_NOT_NULL(srv->tls_config_slots[1]);
    ASSERT_EQ(srv->tls_config_count, 2);
    /* Slots 2 and 3 are NULL (only 2 listeners requested). */
    ASSERT_NULL(srv->tls_config_slots[2]);
    ASSERT_NULL(srv->tls_config_slots[3]);

    /* The two slots must be distinct objects. */
    ASSERT(srv->tls_config_slots[0] != srv->tls_config_slots[1]);

    cmq_server_destroy(srv);
    int rc __attribute__((unused)) = system("rm -rf " P5_TEST_DIR);
    (void)rc;
}

/* v0.5.32: per-listener TLS slot lookup. The accept callback must
 * pick the right tls_config_slot for each listen fd, so a
 * connection arriving on listen_fds[i] uses tls_config_slots[i].
 * This test verifies the helper by feeding each listen fd and
 * asserting the returned slot index. The listen fds are populated
 * by cmq_server_run, so we run the server on a thread, wait for
 * the bind, then check. */
#include <pthread.h>
static void *p5_server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

TEST(p5, srv_find_tls_slot_lookup) {
    ensure_dir();
    gen_cert(P5_TEST_DIR "/cert0.pem", P5_TEST_DIR "/key0.pem", "p5server0");
    gen_cert(P5_TEST_DIR "/cert1.pem", P5_TEST_DIR "/key1.pem", "p5server1");
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25002;
    cfg.log_to_stdout = 0;
    cfg.tls_enabled = 1;
    cfg.tls_cert = P5_TEST_DIR "/cert0.pem";
    cfg.tls_key = P5_TEST_DIR "/key0.pem";
    cfg.listeners[1].tls_cert = P5_TEST_DIR "/cert1.pem";
    cfg.listeners[1].tls_key = P5_TEST_DIR "/key1.pem";
    cfg.listener_count = 2;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Spin the server in a background thread; it binds listen_fds
     * during cmq_server_run. */
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, p5_server_thread, srv), 0);
    /* Poll for the bind (max 1 second). */
    for (int i = 0; i < 100; i++) {
        if (srv->listen_fds[0] >= 0 && srv->listen_fds[1] >= 0) break;
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }

    int s0 = srv->listen_fds[0];
    int s1 = srv->listen_fds[1];
    ASSERT(s0 >= 0);
    ASSERT(s1 >= 0);
    ASSERT(s0 != s1);

    /* srv_find_tls_slot is declared in cmq_server.h (v0.5.32). */
    extern int srv_find_tls_slot(cmq_server_t *srv, int lfd);
    ASSERT_EQ(srv_find_tls_slot(srv, s0), 0);
    ASSERT_EQ(srv_find_tls_slot(srv, s1), 1);
    /* Unknown fd falls back to slot 0. */
    ASSERT_EQ(srv_find_tls_slot(srv, -1), 0);
    ASSERT_EQ(srv_find_tls_slot(srv, 99999), 0);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " P5_TEST_DIR);
    (void)rc2;
}

TEST_MAIN()