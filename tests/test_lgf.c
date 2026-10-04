/* v0.5.133: empty log_file / slot-0 TLS store NULL. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_lgf.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(lgf, apply) {
    const char *path = write_conf(
        "log_file = /tmp/cmq_lgf.log\n"
        "tls_cert = /tmp/c.pem\n"
        "tls_key = /tmp/k.pem\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.log_file, "/tmp/cmq_lgf.log");
    ASSERT_STR_EQ(cfg.tls_cert, "/tmp/c.pem");
    ASSERT_STR_EQ(cfg.tls_key, "/tmp/k.pem");
    cmq_config_free(&cfg);
}

TEST(lgf, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.log_file == NULL);
    ASSERT(cfg.tls_cert == NULL);
    ASSERT(cfg.tls_key == NULL);
    cmq_config_free(&cfg);
}

TEST(lgf, empty) {
    const char *path = write_conf(
        "log_file =\n"
        "tls_cert =\n"
        "tls_key =\n"
        "jwks_url =\n"
        "otlp_endpoint =\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.log_file == NULL);
    ASSERT(cfg.tls_cert == NULL);
    ASSERT(cfg.tls_key == NULL);
    ASSERT(cfg.jwks_url == NULL);
    ASSERT(cfg.otlp_endpoint == NULL);
    cmq_config_free(&cfg);
}

TEST(lgf, reject) {
    const char *path = write_conf("log_file = ../evil.log\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("log_file = bad\\path.log\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
