/* v0.5.57: MQTT outbound QoS 2 PUBLISH / PUBREC / PUBREL / PUBCOMP. */
#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

TEST(qos2, offer_encode) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "ab", (const uint8_t *)"xy", 2, 2,
                                      &id),
              0);
    ASSERT(id != 0);
    uint8_t pkt[64];
    size_t n = 0;
    ASSERT_EQ(cmq_mqtt_inflight_encode(&w, id, pkt, sizeof(pkt), &n), 0);
    ASSERT(n >= 8);
    ASSERT_EQ(pkt[0], (uint8_t)0x34);
    ASSERT_EQ(pkt[2], (uint8_t)0);
    ASSERT_EQ(pkt[3], (uint8_t)2);
    ASSERT(memcmp(pkt + 4, "ab", 2) == 0);
    ASSERT_EQ(pkt[6], (uint8_t)(id >> 8));
    ASSERT_EQ(pkt[7], (uint8_t)(id & 0xFF));
    ASSERT(memcmp(pkt + 8, "xy", 2) == 0);
}

TEST(qos2, handshake_order) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 2, &id),
              0);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), -1);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    ASSERT_EQ(cmq_mqtt_inflight_rec(&w, id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    ASSERT_EQ(cmq_mqtt_inflight_rec(&w, id), -1);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 0);
}

TEST(qos2, rec_rejects_qos1) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 1, &id),
              0);
    ASSERT_EQ(cmq_mqtt_inflight_rec(&w, id), -1);
    ASSERT_EQ(cmq_mqtt_inflight_ack(&w, id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_rec(&w, 0), -1);
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 0, &id),
              -1);
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 3, &id),
              -1);
}

TEST(qos2, encode_pubrel) {
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    uint16_t id = 0;
    ASSERT_EQ(cmq_mqtt_inflight_offer(&w, "t", (const uint8_t *)"z", 1, 2, &id),
              0);
    ASSERT_EQ(cmq_mqtt_inflight_rec(&w, id), 0);
    uint8_t pkt[8];
    size_t n = 0;
    ASSERT_EQ(cmq_mqtt_inflight_encode_pubrel(&w, id, pkt, sizeof(pkt), &n), 0);
    ASSERT_EQ(n, (size_t)4);
    ASSERT_EQ(pkt[0], (uint8_t)0x62);
    ASSERT_EQ(pkt[1], (uint8_t)0x02);
    ASSERT_EQ(pkt[2], (uint8_t)(id >> 8));
    ASSERT_EQ(pkt[3], (uint8_t)(id & 0xFF));
}

TEST(qos2, fanout_handshake) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    ASSERT_EQ(cmq_mqtt_session_attach(sv[0], &w), 0);
    ASSERT_EQ(cmq_mqtt_session_add_filter(sv[0], "sensors/#", 2), 0);
    ASSERT_EQ(cmq_mqtt_fanout("sensors/t", (const uint8_t *)"ok", 2), 1);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    uint8_t pkt[64];
    ssize_t n = recv(sv[1], pkt, sizeof(pkt), 0);
    ASSERT(n >= 8);
    ASSERT_EQ(pkt[0], (uint8_t)0x34);
    size_t tlen = ((size_t)pkt[2] << 8) | pkt[3];
    ASSERT_EQ(tlen, (size_t)9);
    uint16_t id = ((uint16_t)pkt[4 + tlen] << 8) | pkt[5 + tlen];
    ASSERT_EQ(cmq_mqtt_session_rec(sv[0], id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 1);
    uint8_t rel[8];
    ssize_t rn = recv(sv[1], rel, sizeof(rel), 0);
    ASSERT_EQ(rn, (ssize_t)4);
    ASSERT_EQ(rel[0], (uint8_t)0x62);
    ASSERT_EQ(rel[1], (uint8_t)0x02);
    ASSERT_EQ(cmq_mqtt_session_ack(sv[0], id), 0);
    ASSERT_EQ(cmq_mqtt_inflight_count(&w), 0);
    cmq_mqtt_session_detach(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

TEST(qos2, qos1_untouched) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    cmq_mqtt_inflight_t w;
    cmq_mqtt_inflight_init(&w);
    ASSERT_EQ(cmq_mqtt_session_attach(sv[0], &w), 0);
    ASSERT_EQ(cmq_mqtt_session_add_filter(sv[0], "t", 1), 0);
    ASSERT_EQ(cmq_mqtt_fanout("t", (const uint8_t *)"x", 1), 1);
    uint8_t pkt[32];
    ssize_t n = recv(sv[1], pkt, sizeof(pkt), 0);
    ASSERT(n >= 6);
    ASSERT_EQ(pkt[0], (uint8_t)0x32);
    ASSERT_EQ(cmq_mqtt_session_rec(sv[0], 1), -1);
    cmq_mqtt_session_detach(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

TEST_MAIN()
