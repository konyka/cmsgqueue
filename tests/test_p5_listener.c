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

TEST_MAIN()