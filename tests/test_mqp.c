/* v0.5.112: MQTT bridge outbound PUBLISH on mapped subjects. */
#include "cmq_test.h"
#include "cmq_mqtt.h"
#include "cmq_config.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int listen_loopback(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    socklen_t n = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &n) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(sa.sin_port);
    return fd;
}

struct dummy {
    int lfd;
    uint8_t got[256];
    size_t gotn;
};

static void *dummy_main(void *arg) {
    struct dummy *d = arg;
    int cfd = accept(d->lfd, NULL, NULL);
    if (cfd < 0) return NULL;
    uint8_t tmp[512];
    (void)recv(cfd, tmp, sizeof(tmp), 0);
    uint8_t ack[] = { 0x20, 0x02, 0x00, 0x00 };
    (void)send(cfd, ack, 4, 0);
    ssize_t n = recv(cfd, d->got, sizeof(d->got), 0);
    d->gotn = n > 0 ? (size_t)n : 0;
    close(cfd);
    return NULL;
}

static const char *write_conf(const char *content) {
    const char *path = "/tmp/cmq_test_mqp.conf";
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;
    fputs(content, fp);
    fclose(fp);
    return path;
}

TEST(mqp, hit) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct dummy d;
    memset(&d, 0, sizeof(d));
    d.lfd = lfd;
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, dummy_main, &d), 0);
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqp");
    ASSERT_NOT_NULL(br);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.temp", "sensor/temp", 0), 0);
    ASSERT_EQ(cmq_mqtt_bridge_connect(br, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(br, "sensor.temp",
                                      (const uint8_t *)"22", 2), 1);
    pthread_join(th, NULL);
    close(lfd);
    ASSERT(d.gotn >= 3);
    ASSERT_EQ((int)(d.got[0] & 0xF0), 0x30);
    cmq_mqtt_bridge_info_t info = cmq_mqtt_bridge_info(br);
    ASSERT_EQ((int)info.messages_out, 1);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqp, miss) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqp");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "a", "a/x", 0), 0);
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct dummy d;
    memset(&d, 0, sizeof(d));
    d.lfd = lfd;
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, dummy_main, &d), 0);
    ASSERT_EQ(cmq_mqtt_bridge_connect(br, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(br, "other",
                                      (const uint8_t *)"x", 1), 0);
    cmq_mqtt_bridge_disconnect(br);
    pthread_join(th, NULL);
    close(lfd);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqp, disconnected) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqp");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "a", "a/x", 0), 0);
    ASSERT(cmq_mqtt_bridge_publish(br, "a", (const uint8_t *)"x", 1) != 1);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqp, reject) {
    ASSERT(cmq_mqtt_bridge_publish(NULL, "a", (const uint8_t *)"x", 1) != 1);
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqp");
    ASSERT(cmq_mqtt_bridge_publish(br, NULL, (const uint8_t *)"x", 1) != 1);
    ASSERT(cmq_mqtt_bridge_publish(br, "a", NULL, 3) != 1);
    const char *path = write_conf(
        "mqtt_bridge_map = sensor.temp,sensor/temp,0\n"
        "mqtt_bridge_map = bad/slash,ok\n"
    );
    cmq_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT(cmq_config_load(path, &cfg) != CMQ_OK);
    cmq_config_free(&cfg);
    path = write_conf("mqtt_bridge_map = sensor.temp,sensor/temp,0\n");
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cmq_config_load(path, &cfg), CMQ_OK);
    ASSERT_EQ(cfg.mqtt_bridge_map_count, 1);
    ASSERT_STR_EQ(cfg.mqtt_bridge_maps[0].cmq_subject, "sensor.temp");
    ASSERT_STR_EQ(cfg.mqtt_bridge_maps[0].mqtt_topic, "sensor/temp");
    ASSERT_EQ(cfg.mqtt_bridge_maps[0].qos, 0);
    cmq_config_free(&cfg);
    cmq_mqtt_bridge_destroy(br);
}

TEST_MAIN()
