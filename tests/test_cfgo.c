/* v0.5.113: tls_ca / acl_* / blocklist_file are owned config strings. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_cfgo.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(cfgo, apply) {
    const char *path = write_conf(
        "tls_ca = /tmp/ca.pem\n"
        "acl_allow = foo.>\n"
        "acl_deny = secret.>\n"
        "blocklist_file = /tmp/block.txt\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.tls_ca, "/tmp/ca.pem");
    ASSERT_STR_EQ(cfg.acl_allow, "foo.>");
    ASSERT_STR_EQ(cfg.acl_deny, "secret.>");
    ASSERT_STR_EQ(cfg.blocklist_file, "/tmp/block.txt");
    cmq_config_free(&cfg);
}

TEST(cfgo, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.tls_ca == NULL);
    ASSERT(cfg.acl_allow == NULL);
    ASSERT(cfg.acl_deny == NULL);
    ASSERT(cfg.blocklist_file == NULL);
    cmq_config_free(&cfg);
}

TEST(cfgo, empty) {
    const char *path = write_conf(
        "tls_ca =\n"
        "acl_allow =\n"
        "acl_deny =\n"
        "blocklist_file =\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.tls_ca == NULL);
    ASSERT(cfg.acl_allow == NULL);
    ASSERT(cfg.acl_deny == NULL);
    ASSERT(cfg.blocklist_file == NULL);
    cmq_config_free(&cfg);
}

TEST(cfgo, reload_clears) {
    const char *path = write_conf(
        "acl_allow = foo.>\n"
        "tls_ca = /tmp/ca.pem\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.acl_allow, "foo.>");
    path = write_conf("port = 1\n");
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.acl_allow == NULL);
    ASSERT(cfg.tls_ca == NULL);
    cmq_config_free(&cfg);
}

TEST_MAIN()
