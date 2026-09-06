/* v0.5.118: config_file ownership + SIGHUP pending flag. */
#include "cmq_config.h"
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_hup.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(hup, apply) {
    const char *path = write_conf("config_file = /tmp/cmq_hup_named.conf\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.config_file, "/tmp/cmq_hup_named.conf");
    cmq_config_free(&cfg);
    cmq_sighup_note();
    ASSERT_EQ(cmq_sighup_take(), 1);
    ASSERT_EQ(cmq_sighup_take(), 0);
}

TEST(hup, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.config_file, path);
    cmq_config_free(&cfg);
}

TEST(hup, empty) {
    const char *path = write_conf("config_file =\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.config_file == NULL);
    cmq_config_free(&cfg);
}

TEST(hup, reject) {
    const char *path = write_conf("config_file = ../x.conf\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
