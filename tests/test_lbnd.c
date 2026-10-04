/* v0.5.115: listener[1..3] bind host/port in cmq.conf. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_lbnd.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(lbnd, apply) {
    const char *path = write_conf(
        "listener_count = 2\n"
        "listener1_host = 0.0.0.0\n"
        "listener1_port = 7655\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.listener_count, 2);
    ASSERT_STR_EQ(cfg.listeners[1].host, "0.0.0.0");
    ASSERT_EQ(cfg.listeners[1].port, 7655);
    ASSERT(cfg.listeners[2].host == NULL);
    ASSERT_EQ(cfg.listeners[2].port, 0);
    cmq_config_free(&cfg);
}

TEST(lbnd, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.listeners[1].host == NULL);
    ASSERT_EQ(cfg.listeners[1].port, 0);
    cmq_config_free(&cfg);
}

TEST(lbnd, empty) {
    const char *path = write_conf(
        "listener1_host =\n"
        "listener1_port = 0\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.listeners[1].host == NULL);
    ASSERT_EQ(cfg.listeners[1].port, 0);
    cmq_config_free(&cfg);
}

TEST(lbnd, reject) {
    const char *path = write_conf("listener1_port = 65536\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("listener1_host = localhost\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
