/* v0.5.110: persist_sync_interval_ms is a config-file key. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_psyn.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(psyn, apply) {
    const char *path = write_conf(
        "persist_dir = /tmp/cmq_psyn\n"
        "persist_sync_interval_ms = 1000\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ((int)cfg.persist_sync_interval_ms, 1000);
    cmq_config_free(&cfg);
}

TEST(psyn, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ((int)cfg.persist_sync_interval_ms, 0);
    cmq_config_free(&cfg);
}

TEST(psyn, zero) {
    const char *path = write_conf("persist_sync_interval_ms = 0\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ((int)cfg.persist_sync_interval_ms, 0);
    cmq_config_free(&cfg);
}

TEST(psyn, reject) {
    const char *path = write_conf("persist_sync_interval_ms = -1\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("persist_sync_interval_ms = 86400001\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
