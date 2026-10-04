/* v0.5.46: MQTT CONNECT parse, last-will, durable sessions. */
#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static size_t put_str(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)(n & 0xFF);
    memcpy(p + 2, s, n);
    return 2 + n;
}

static size_t put_connect_hdr(uint8_t *p, uint8_t level, uint8_t flags) {
    p[0] = 0x00;
    p[1] = 0x04;
    memcpy(p + 2, "MQTT", 4);
    p[6] = level;
    p[7] = flags;
    p[8] = 0x00;
    p[9] = 0x3C;
    return 10;
}

TEST(mqtt_will, parse_basic_311) {
    uint8_t buf[64];
    size_t n = put_connect_hdr(buf, 0x04, 0x02);
    n += put_str(buf + n, "cid1");
    cmq_mqtt_connect_info_t ci;
    ASSERT_EQ(cmq_mqtt_parse_connect(buf, n, &ci), 0);
    ASSERT_EQ(ci.is_v5, 0);
    ASSERT_EQ(ci.clean_session, 1);
    ASSERT_EQ(ci.will_flag, 0);
    ASSERT_EQ(strcmp(ci.client_id, "cid1"), 0);
    ASSERT_EQ(ci.username[0], 0);
}

TEST(mqtt_will, parse_will_user_pass) {
    uint8_t buf[128];
    /* clean + will + qos0 + user + pass */
    uint8_t flags = 0x02 | 0x04 | 0x80 | 0x40;
    size_t n = put_connect_hdr(buf, 0x04, flags);
    n += put_str(buf + n, "cid");
    n += put_str(buf + n, "w/topic");
    n += put_str(buf + n, "bye");
    n += put_str(buf + n, "alice");
    n += put_str(buf + n, "secret");
    cmq_mqtt_connect_info_t ci;
    ASSERT_EQ(cmq_mqtt_parse_connect(buf, n, &ci), 0);
    ASSERT_EQ(ci.will_flag, 1);
    ASSERT_EQ(strcmp(ci.will_topic, "w/topic"), 0);
    ASSERT_EQ(ci.will_payload_len, 3);
    ASSERT_MEM_EQ(ci.will_payload, "bye", 3);
    ASSERT_EQ(strcmp(ci.username, "alice"), 0);
    ASSERT_EQ(strcmp(ci.password, "secret"), 0);
}

TEST(mqtt_will, parse_v5_will_props) {
    uint8_t buf[128];
    uint8_t flags = 0x04;
    size_t n = put_connect_hdr(buf, 0x05, flags);
    buf[n++] = 0x00; /* CONNECT props empty */
    n += put_str(buf + n, "c");
    buf[n++] = 0x00; /* Will props empty */
    n += put_str(buf + n, "wt");
    n += put_str(buf + n, "wp");
    cmq_mqtt_connect_info_t ci;
    ASSERT_EQ(cmq_mqtt_parse_connect(buf, n, &ci), 0);
    ASSERT_EQ(ci.is_v5, 1);
    ASSERT_EQ(ci.will_flag, 1);
    ASSERT_EQ(strcmp(ci.will_topic, "wt"), 0);
}

TEST(mqtt_will, parse_rejects_reserved) {
    uint8_t buf[32];
    size_t n = put_connect_hdr(buf, 0x04, 0x01);
    n += put_str(buf + n, "c");
    cmq_mqtt_connect_info_t ci;
    ASSERT_EQ(cmq_mqtt_parse_connect(buf, n, &ci), -1);
}

TEST(mqtt_will, store_take_once) {
    cmq_mqtt_will_clear("w1");
    cmq_mqtt_connect_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.will_flag = 1;
    strcpy(ci.will_topic, "status");
    ci.will_payload = (const uint8_t *)"off";
    ci.will_payload_len = 3;
    ci.will_retain = 1;
    ASSERT_EQ(cmq_mqtt_will_store("w1", &ci), 0);
    char topic[128];
    uint8_t *pl = NULL;
    size_t plen = 0;
    int retain = 0;
    ASSERT_EQ(cmq_mqtt_will_take("w1", topic, sizeof(topic), &pl, &plen, &retain), 0);
    ASSERT_EQ(strcmp(topic, "status"), 0);
    ASSERT_EQ(plen, (size_t)3);
    ASSERT_MEM_EQ(pl, "off", 3);
    ASSERT_EQ(retain, 1);
    free(pl);
    ASSERT_EQ(cmq_mqtt_will_take("w1", topic, sizeof(topic), &pl, &plen, &retain), -1);
}

TEST(mqtt_will, fire_retains) {
    cmq_mqtt_will_clear("w2");
    cmq_mqtt_connect_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.will_flag = 1;
    strcpy(ci.will_topic, "last/will");
    ci.will_payload = (const uint8_t *)"gone";
    ci.will_payload_len = 4;
    ci.will_retain = 1;
    ASSERT_EQ(cmq_mqtt_will_store("w2", &ci), 0);
    ASSERT_EQ(cmq_mqtt_will_fire("w2"), 1);
    const uint8_t *rp = NULL;
    size_t rlen = 0;
    ASSERT_EQ(cmq_mqtt_fetch_retained("last/will", &rp, &rlen), 0);
    ASSERT_EQ(rlen, (size_t)4);
    ASSERT_MEM_EQ(rp, "gone", 4);
    ASSERT_EQ(cmq_mqtt_will_fire("w2"), 0);
}

TEST(mqtt_will, session_save_load_drop) {
    cmq_mqtt_session_drop("s1");
    const char *subs[2] = { "a/b", "c/#" };
    ASSERT_EQ(cmq_mqtt_session_save("s1", subs, 2), 0);
    char out[8][128];
    ASSERT_EQ(cmq_mqtt_session_load("s1", out, 8), 2);
    ASSERT_EQ(strcmp(out[0], "a/b"), 0);
    ASSERT_EQ(strcmp(out[1], "c/#"), 0);
    cmq_mqtt_session_drop("s1");
    ASSERT_EQ(cmq_mqtt_session_load("s1", out, 8), 0);
}

TEST_MAIN()
