/* v0.5.43: MQTT 5.0 property decode + PUBLISH payload offset. */
#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <string.h>
#include <stdint.h>

TEST(mqtt_props, empty_list) {
    uint8_t buf[] = { 0x00 }; /* property length 0 */
    cmq_mqtt_props_t p;
    ASSERT_EQ(cmq_mqtt_props_decode(buf, sizeof(buf), &p), 0);
    ASSERT_EQ(p.consumed, 1);
    ASSERT_EQ(p.payload_format, 0xFF);
    ASSERT_EQ(p.content_type_len, 0);
}

TEST(mqtt_props, payload_format_and_content_type) {
    /* props_len=9: 0x01 0x01 | 0x03 0x00 0x04 json */
    uint8_t buf[] = {
        0x09,
        0x01, 0x01,
        0x03, 0x00, 0x04, 'j', 's', 'o', 'n'
    };
    cmq_mqtt_props_t p;
    ASSERT_EQ(cmq_mqtt_props_decode(buf, sizeof(buf), &p), 0);
    ASSERT_EQ(p.payload_format, 1);
    ASSERT_EQ(p.content_type_len, 4);
    ASSERT_MEM_EQ(p.content_type, "json", 4);
}

TEST(mqtt_props, truncated_rejected) {
    uint8_t buf[] = { 0x05, 0x03, 0x00, 0x04, 'j' };
    cmq_mqtt_props_t p;
    ASSERT_EQ(cmq_mqtt_props_decode(buf, sizeof(buf), &p), -1);
}

TEST(mqtt_props, unknown_id_rejected) {
    uint8_t buf[] = { 0x02, 0xFE, 0x00 };
    cmq_mqtt_props_t p;
    ASSERT_EQ(cmq_mqtt_props_decode(buf, sizeof(buf), &p), -1);
}

TEST(mqtt_props, publish_off_qos0_v311) {
    uint8_t vh[] = { 0x00, 0x01, 'a', 'h', 'i' };
    cmq_mqtt_props_t p;
    ssize_t off = cmq_mqtt_publish_payload_off(vh, sizeof(vh), 0, 0, &p);
    ASSERT_EQ(off, 3);
}

TEST(mqtt_props, publish_off_qos0_v5_empty_props) {
    uint8_t vh[] = { 0x00, 0x01, 'a', 0x00, 'h', 'i' };
    cmq_mqtt_props_t p;
    ssize_t off = cmq_mqtt_publish_payload_off(vh, sizeof(vh), 0, 1, &p);
    ASSERT_EQ(off, 4);
}

TEST(mqtt_props, publish_off_qos1_v5_with_format) {
    /* topic "a", pid 0x0001, props_len=2 (format=1), payload "xy" */
    uint8_t vh[] = {
        0x00, 0x01, 'a',
        0x00, 0x01,
        0x02, 0x01, 0x01,
        'x', 'y'
    };
    cmq_mqtt_props_t p;
    ssize_t off = cmq_mqtt_publish_payload_off(vh, sizeof(vh), 1, 1, &p);
    ASSERT_EQ(off, 8);
    ASSERT_EQ(p.payload_format, 1);
}

TEST_MAIN()
