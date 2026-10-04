/* v0.5.109: persist_dir is a config-file key. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_pdir.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(pdir, apply) {
    const char *path = write_conf(
        "port = 7654\n"
        "persist_dir = /tmp/cmq_pdir\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.persist_dir, "/tmp/cmq_pdir");
    cmq_config_free(&cfg);
}

TEST(pdir, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.persist_dir == NULL);
    cmq_config_free(&cfg);
}

TEST(pdir, empty) {
    const char *path = write_conf("persist_dir =\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.persist_dir == NULL);
    cmq_config_free(&cfg);
}

TEST(pdir, reject) {
    const char *path = write_conf("persist_dir = /tmp/../etc\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
