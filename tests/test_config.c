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
    cmq_config_free(&config);
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
    cmq_config_free(&config);
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
    cmq_config_free(&config);
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
    cmq_config_free(&config);
}

TEST(config, log_level_names_and_trace) {
    const char *path = write_test_config("log_level = debug\n");
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_EQ(config.log_level, 1);
    cmq_config_free(&config);

    path = write_test_config("log_level = TRACE\n");
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_EQ(config.log_level, 0);
    cmq_config_free(&config);

    path = write_test_config("log_level = 0\n");
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_EQ(config.log_level, 0);
    cmq_config_free(&config);

    path = write_test_config("host = 127.0.0.1\n");
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_EQ(config.log_level, 2); /* omitted → INFO */
    cmq_config_free(&config);
}

TEST(config, load_file_not_found) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load("/nonexistent/path.conf", &config);
    ASSERT(rc != CMQ_OK);
}

TEST(config, load_resets_unspecified) {
    const char *path = "/tmp/cmq_test_config_reset.conf";
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL);
    fprintf(fp, "host = 127.0.0.1\n");
    fclose(fp);

    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 65535;
    config.max_clients = 12345;
    config.num_threads = 9;
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_EQ(config.port, 0);
    ASSERT_EQ(config.max_clients, 0);
    ASSERT_EQ(config.num_threads, 0);
    ASSERT(config.host && strcmp(config.host, "127.0.0.1") == 0);
    cmq_config_free(&config);
    unlink(path);
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

TEST(config, validate_negative_limits) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.max_clients = -1;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.max_clients = 0;
    config.write_timeout_ms = -1;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
}

TEST(config, validate_routes_need_cluster) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.route_count = 1;
    config.routes[0].addr = strdup("10.0.0.1");
    config.routes[0].port = 7654;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.cluster_name = strdup("c1");
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.cluster_node_id = strdup("n1");
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    cmq_config_free(&config);
}

TEST(config, validate_routes_unique_ip) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.cluster_name = strdup("c1");
    config.cluster_node_id = strdup("n1");
    config.route_count = 2;
    config.routes[0].addr = strdup("10.0.0.1");
    config.routes[0].port = 7654;
    config.routes[1].addr = strdup("10.0.0.1");
    config.routes[1].port = 7655;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    free((void *)config.routes[1].addr);
    config.routes[1].addr = strdup("10.0.0.2");
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    cmq_config_free(&config);
}

TEST(config, validate_routes_bad_addr) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.cluster_name = strdup("c1");
    config.cluster_node_id = strdup("n1");
    config.route_count = 1;
    config.routes[0].addr = strdup("not-an-ip");
    config.routes[0].port = 7654;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    cmq_config_free(&config);
}

TEST(config, validate_route_count_cap) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.cluster_name = strdup("c1");
    config.cluster_node_id = strdup("n1");
    config.route_count = 9; /* would walk past routes[8] into adjacent fields */
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    cmq_config_free(&config);
}

TEST(config, validate_route_bad_port) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.cluster_name = strdup("c1");
    config.cluster_node_id = strdup("n1");
    config.route_count = 1;
    config.routes[0].addr = strdup("10.0.0.1");
    config.routes[0].port = 0;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.routes[0].port = 70000;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.routes[0].port = 7654;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    cmq_config_free(&config);
}

TEST(config, validate_auth_cred_length) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    char longcred[300];
    memset(longcred, 'a', sizeof(longcred) - 1);
    longcred[sizeof(longcred) - 1] = '\0';
    config.auth_password = longcred;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.auth_password = "ok";
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
}

TEST(config, validate_username_requires_password) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.auth_username = "admin";
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.auth_password = "secret";
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    /* Password-only remains allowed. */
    config.auth_username = NULL;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
}

TEST(config, validate_ping_write_timeout_cap) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.ping_interval_ms = 86400000;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    config.ping_interval_ms = 86400001;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.ping_interval_ms = 30000;
    config.write_timeout_ms = 86400001;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
}

TEST(config, validate_tls_requires_certs) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.tls_enabled = 1;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.tls_cert = "/tmp/cert.pem";
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
    config.tls_key = "/tmp/key.pem";
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
}

TEST(config, validate_max_payload_cap) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.max_payload_size = CMQ_MAX_PAYLOAD_LIMIT;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    config.max_payload_size = CMQ_MAX_PAYLOAD_LIMIT + 1;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
}

TEST(config, validate_max_subs_cap) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    config.max_subs_per_client = CMQ_DEFAULT_MAX_SUBS_PER_CLIENT;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    config.max_subs_per_client = CMQ_DEFAULT_MAX_SUBS_PER_CLIENT + 1;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
}

TEST(config, validate_cluster_id_len) {
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = 7654;
    char ok_id[16];
    memset(ok_id, 'a', 15);
    ok_id[15] = '\0';
    config.cluster_node_id = ok_id;
    ASSERT_EQ(cmq_config_validate(&config), CMQ_OK);
    char long_id[17];
    memset(long_id, 'b', 16);
    long_id[16] = '\0';
    config.cluster_node_id = long_id;
    ASSERT(cmq_config_validate(&config) != CMQ_OK);
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

TEST(config, load_cluster_tls_routes) {
    const char *path = write_test_config(
        "cluster_name = c1\n"
        "cluster_node_id = n1\n"
        "tls_enabled = 1\n"
        "tls_cert = /tmp/cert.pem\n"
        "tls_key = /tmp/key.pem\n"
        "route = 10.0.0.1:7654\n"
        "route = 10.0.0.2:7655\n"
    );
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(cmq_config_load(path, &config), CMQ_OK);
    ASSERT_STR_EQ(config.cluster_name, "c1");
    ASSERT_STR_EQ(config.cluster_node_id, "n1");
    ASSERT_EQ(config.tls_enabled, 1);
    ASSERT_STR_EQ(config.tls_cert, "/tmp/cert.pem");
    ASSERT_STR_EQ(config.tls_key, "/tmp/key.pem");
    ASSERT_EQ(config.route_count, 2);
    ASSERT_STR_EQ(config.routes[0].addr, "10.0.0.1");
    ASSERT_EQ(config.routes[0].port, 7654);
    ASSERT_STR_EQ(config.routes[1].addr, "10.0.0.2");
    ASSERT_EQ(config.routes[1].port, 7655);
    cmq_config_free(&config);
}

TEST(config, load_empty_file) {
    const char *path = write_test_config("");
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_OK);
}

TEST(config, reject_overlong_line) {
    char buf[1200];
    memset(buf, 'a', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    /* key= + 1100-char value without newline mid-stream → truncated fgets. */
    char content[1300];
    snprintf(content, sizeof(content), "tls_cert = %s\nport = 4222\n", buf);
    const char *path = write_test_config(content);
    cmq_config_t config;
    memset(&config, 0, sizeof(config));
    cmq_status_t rc = cmq_config_load(path, &config);
    ASSERT_EQ(rc, CMQ_ERR_INVALID_ARG);
}

TEST_MAIN()
