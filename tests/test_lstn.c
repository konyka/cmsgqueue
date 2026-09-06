/* v0.5.114: listener[1..3] TLS keys in cmq.conf + owned strings. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_lstn.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(lstn, apply) {
    const char *path = write_conf(
        "listener_count = 2\n"
        "listener1_tls_cert = /tmp/c1.pem\n"
        "listener1_tls_key = /tmp/k1.pem\n"
        "listener1_tls_ca = /tmp/ca1.pem\n"
        "listener1_tls_verify_peer = 1\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.listener_count, 2);
    ASSERT_STR_EQ(cfg.listeners[1].tls_cert, "/tmp/c1.pem");
    ASSERT_STR_EQ(cfg.listeners[1].tls_key, "/tmp/k1.pem");
    ASSERT_STR_EQ(cfg.listeners[1].tls_ca, "/tmp/ca1.pem");
    ASSERT_EQ(cfg.listeners[1].tls_verify_peer, 1);
    ASSERT(cfg.listeners[2].tls_cert == NULL);
    cmq_config_free(&cfg);
}

TEST(lstn, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.listener_count, 0);
    ASSERT(cfg.listeners[1].tls_cert == NULL);
    ASSERT(cfg.listeners[1].tls_key == NULL);
    cmq_config_free(&cfg);
}

TEST(lstn, empty) {
    const char *path = write_conf(
        "listener1_tls_cert =\n"
        "listener1_tls_key =\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.listeners[1].tls_cert == NULL);
    ASSERT(cfg.listeners[1].tls_key == NULL);
    cmq_config_free(&cfg);
}

TEST(lstn, reject) {
    const char *path = write_conf("listener_count = 5\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("listener1_tls_verify_peer = 2\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
