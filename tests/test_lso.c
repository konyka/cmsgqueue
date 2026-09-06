/* v0.5.128: omitted log_to_stdout defaults to 1. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_lso.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(lso, apply) {
    const char *path = write_conf("log_to_stdout = 0\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.log_to_stdout, 0);
    cmq_config_free(&cfg);
}

TEST(lso, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.log_to_stdout, 1);
    cmq_config_free(&cfg);
}

TEST(lso, empty) {
    const char *path = write_conf("log_to_stdout =\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.log_to_stdout, 0);
    cmq_config_free(&cfg);
}

TEST(lso, reject) {
    const char *path = write_conf("log_to_stdout = 2\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
