/* v0.5.132: empty host is NULL; non-IPv4 fails closed. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_hst.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(hst, apply) {
    const char *path = write_conf("host = 127.0.0.1\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.host, "127.0.0.1");
    cmq_config_free(&cfg);
}

TEST(hst, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.host == NULL);
    cmq_config_free(&cfg);
}

TEST(hst, empty) {
    const char *path = write_conf("host =\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.host == NULL);
    cmq_config_free(&cfg);
}

TEST(hst, reject) {
    const char *path = write_conf("host = localhost\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
