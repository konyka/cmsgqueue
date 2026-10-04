/* v0.5.111: mqtt_bridge_addr / mqtt_bridge_port are config-file keys. */
#include "cmq_config.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_mqb.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(mqb, apply) {
    const char *path = write_conf(
        "mqtt_bridge_addr = 127.0.0.1\n"
        "mqtt_bridge_port = 1883\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_STR_EQ(cfg.mqtt_bridge_addr, "127.0.0.1");
    ASSERT_EQ(cfg.mqtt_bridge_port, 1883);
    cmq_config_free(&cfg);
}

TEST(mqb, omitted) {
    const char *path = write_conf("port = 7654\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.mqtt_bridge_addr == NULL);
    ASSERT_EQ(cfg.mqtt_bridge_port, 0);
    cmq_config_free(&cfg);
}

TEST(mqb, empty) {
    const char *path = write_conf(
        "mqtt_bridge_addr =\n"
        "mqtt_bridge_port = 0\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT(cfg.mqtt_bridge_addr == NULL);
    ASSERT_EQ(cfg.mqtt_bridge_port, 0);
    cmq_config_free(&cfg);
}

TEST(mqb, reject) {
    const char *path = write_conf("mqtt_bridge_port = 65536\n");
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("mqtt_bridge_addr = 127.0.0.1/evil\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
}

TEST_MAIN()
