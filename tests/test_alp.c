/* v0.5.151: reload sets h2 ALPN on an existing TLS slot. */
#include "cmq_test.h"
#include "cmq_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALP_CERT "build-tdd/alp_cert.pem"
#define ALP_KEY  "build-tdd/alp_key.pem"

static int gen_cert(void) {
    (void)system("mkdir -p build-tdd");
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
             "-days 1 -nodes -subj /CN=alp 2>/dev/null",
             ALP_KEY, ALP_CERT);
    return system(cmd) == 0 && access(ALP_CERT, R_OK) == 0 &&
           access(ALP_KEY, R_OK) == 0;
}

TEST(alp, apply) {
    ASSERT(gen_cert());
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_set_cert(cfg, ALP_CERT), 0);
    ASSERT_EQ(cmq_tls_set_key(cfg, ALP_KEY), 0);
    ASSERT_EQ(cmq_tls_load(cfg), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 0);
    ASSERT_EQ(cmq_tls_reload_alpn(cfg, "h2"), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 1);
    ASSERT_EQ(cmq_tls_reload_alpn(cfg, "http/1.1"), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 1);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "http/1.1"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(alp, omitted) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_reload_alpn(cfg, NULL), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(alp, empty) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_reload_alpn(cfg, ""), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(alp, reject) {
    ASSERT(cmq_tls_reload_alpn(NULL, "h2") != 0);
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT(cmq_tls_reload_alpn(cfg, "../evil") != 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST_MAIN()
