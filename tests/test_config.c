#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *write_test_config(const char *content) {
    const char *path = "/tmp/cmq_test_config.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(config, load_basic) {
    const char *path = write_test_config(
        "host = 0.0.0.0\n"
        "port = 4222\n"
        "threads = 4\n"
        "max_clients = 1000\n"
        "max_payload_size = 1048576\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
    ASSERT_STR_EQ(config.host, "0.0.0.0");
    ASSERT_EQ(config.port, 4222);
    ASSERT_EQ(config.num_threads, 4);
    ASSERT_EQ(config.max_clients, 1000);
    ASSERT_EQ(config.max_payload_size, 1048576);
    free((void *)config.host);
}

TEST(config, load_with_comments) {
    const char *path = write_test_config(
        "# Server config\n"
        "port = 7654  # default port\n"
        "\n"
        "  host = 127.0.0.1\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
    ASSERT_EQ(config.port, 7654);
    ASSERT_STR_EQ(config.host, "127.0.0.1");
    free((void *)config.host);
}

TEST(config, load_quoted_values) {
    const char *path = write_test_config(
        "host = \"192.168.1.1\"\n"
        "log_file = \"/var/log/cmq.log\"\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
    ASSERT_STR_EQ(config.host, "192.168.1.1");
    ASSERT_STR_EQ(config.log_file, "/var/log/cmq.log");
    free((void *)config.host);
    free((void *)config.log_file);
}

TEST(config, load_logging) {
    const char *path = write_test_config(
        "log_level = 2\n"
        "log_to_stdout = 1\n"
        "log_to_file = 1\n"
        "log_file = test.log\n"
        "ping_interval_ms = 15000\n"
        "write_timeout_ms = 3000\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
    ASSERT_EQ(config.log_level, 2);
    ASSERT_EQ(config.log_to_stdout, 1);
    ASSERT_EQ(config.log_to_file, 1);
    ASSERT_EQ(config.ping_interval_ms, 15000);
    ASSERT_EQ(config.write_timeout_ms, 3000);
    free((void *)config.log_file);
}

TEST(config, load_file_not_found) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load("/nonexistent/path.conf", &config);
    ASSERT(rc != CMQ_OK);
}

TEST(config, load_null_args) {
    cmq_status_t rc = cmq_config_load(NULL, NULL);
    ASSERT_EQ(rc, CMQ_ERR_INVALID_ARG);
}

TEST(config, validate_ok) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.max_payload_size = 1024;
    config.ping_interval_ms = 30000;
    config.num_threads = 4;
    cmq_status_t rc = cmq_config_validate(&config);
    ASSERT_EQ(rc, CMQ_OK);
}

TEST(config, validate_bad_port) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 99999;
    cmq_status_t rc = cmq_config_validate(&config);
    ASSERT(rc != CMQ_OK);
}

TEST(config, load_skip_sections) {
    const char *path = write_test_config(
        "[server]\n"
        "port = 4222\n"
        "[logging]\n"
        "log_level = 3\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
    ASSERT_EQ(config.port, 4222);
    ASSERT_EQ(config.log_level, 3);
}

TEST(config, load_empty_file) {
    const char *path = write_test_config("");
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
}

TEST_MAIN()
