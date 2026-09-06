/* v0.5.54: MQTT QoS 1 outbound inflight window. */
#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

TEST(inflight, offer_ack) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 0);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"hi", 2, 1, &id),
              0);
    ASSERT(id != 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 0);
}

TEST(inflight, window_full) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    for (int i = 0; i < CMQ_MQTT_INFLIGHT_MAX; i++)
        ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"x", 1, 1,
                                          &id),
                  0);
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"x", 1, 1, &id),
              -2);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), CMQ_MQTT_INFLIGHT_MAX);
}

TEST(inflight, encode_qos1) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "ab", (const uint8_t *)"xy", 2, 1,
                                      &id),
              0);
    uint8_t pkt[64];
    size_t n = 0;
    ASSERT_EQ(cmq_mqtt_inflight_encode(&w, id, pkt, sizeof(pkt), &n), 0);
    ASSERT(n >= 8);
    ASSERT_EQ(pkt[0], (uint8_t)0x32);
    ASSERT_EQ(pkt[2], (uint8_t)0);
    ASSERT_EQ(pkt[3], (uint8_t)2);
    ASSERT(memcmp(pkt + 4, "ab", 2) == 0);
    ASSERT_EQ(pkt[6], (uint8_t)(id >> 8));
    ASSERT_EQ(pkt[7], (uint8_t)(id & 0xFF));
    ASSERT(memcmp(pkt + 8, "xy", 2) == 0);
}

TEST(inflight, dup_ack_noop) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 1, &id),
              0);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), -1);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, 0), -1);
}

TEST(inflight, id_never_zero) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    for (int i = 0; i < 20; i++) {
        uint16_t id = 0;
        ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"a", 1, 1,
                                          &id),
                  0);
        ASSERT(id != 0);
        ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), 0);
    }
}

TEST(inflight, fanout_ack) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    ASSERT_EQ(cmq_mqtt_session_attach(sv[0], &w), 0);
    ASSERT_EQ(cmq_mqtt_session_add_filter(sv[0], "sensors/#", 1), 0);
    ASSERT_EQ(cmq_mqtt_fanout("sensors/t", (const uint8_t *)"ok", 2), 1);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    uint8_t pkt[64];
    ssize_t n = recv(sv[1], pkt, sizeof(pkt), 0);
    ASSERT(n >= 8);
    ASSERT_EQ(pkt[0], (uint8_t)0x32);
    size_t tlen = ((size_t)pkt[2] << 8) | pkt[3];
    ASSERT_EQ(tlen, (size_t)9); /* sensors/t */
    uint16_t id = ((uint16_t)pkt[4 + tlen] << 8) | pkt[5 + tlen];
    ASSERT_EQ(cmq_mqtt_session_ack(sv[0], id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 0);
    cmq_mqtt_session_detach(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

TEST(inflight, fanout_full_skips) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    ASSERT_EQ(cmq_mqtt_session_attach(sv[0], &w), 0);
    ASSERT_EQ(cmq_mqtt_session_add_filter(sv[0], "t", 1), 0);
    for (int i = 0; i < CMQ_MQTT_INFLIGHT_MAX; i++)
        ASSERT_EQ(cmq_mqtt_fanout("t", (const uint8_t *)"x", 1), 1);
    ASSERT_EQ(cmq_mqtt_fanout("t", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), CMQ_MQTT_INFLIGHT_MAX);
    cmq_mqtt_session_detach(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

TEST_MAIN()
