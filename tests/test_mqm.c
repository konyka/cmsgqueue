/* v0.5.126: reload replaces live MQTT bridge maps. */
#include "cmq_mqtt.h"
#include "cmq_test.h"
#include <string.h>

static void fill_map(cmq_mqtt_mapping_t *m, const char *s, const char *t, int qos) {
    memset(m, 0, sizeof(*m));
    snprintf(m->cmq_subject, sizeof(m->cmq_subject), "%s", s);
    snprintf(m->mqtt_topic, sizeof(m->mqtt_topic), "%s", t);
    m->qos = qos;
    m->active = 1;
}

TEST(mqm, apply) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("t");
    ASSERT(br != NULL);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "old.s", "old/t", 0), 0);
    cmq_mqtt_mapping_t maps[2];
    fill_map(&maps[0], "a.s", "a/t", 1);
    fill_map(&maps[1], "b.s", "b/t", 0);
    ASSERT_EQ(cmq_mqtt_reload_maps(br, maps, 2), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)2);
    cmq_mqtt_mapping_t got;
    ASSERT_EQ(cmq_mqtt_find_mapping(br, "a/t", &got), 0);
    ASSERT_STR_EQ(got.cmq_subject, "a.s");
    ASSERT(cmq_mqtt_find_mapping(br, "old/t", &got) != 0);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqm, omitted) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("t");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "keep.s", "keep/t", 0), 0);
    ASSERT_EQ(cmq_mqtt_reload_maps(br, NULL, 0), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)1);
    cmq_mqtt_mapping_t got;
    ASSERT_EQ(cmq_mqtt_find_mapping(br, "keep/t", &got), 0);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqm, empty) {
    ASSERT_EQ(cmq_mqtt_reload_maps(NULL, NULL, 0), 0);
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("t");
    ASSERT_EQ(cmq_mqtt_reload_maps(br, NULL, 0), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)0);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqm, reject) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("t");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "keep.s", "keep/t", 0), 0);
    cmq_mqtt_mapping_t bad;
    fill_map(&bad, "x.s", "x/t", 3);
    ASSERT(cmq_mqtt_reload_maps(br, &bad, 1) != 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)1);
    ASSERT(cmq_mqtt_reload_maps(NULL, &bad, 1) != 0);
    fill_map(&bad, "", "x/t", 0);
    ASSERT(cmq_mqtt_reload_maps(br, &bad, 1) != 0);
    cmq_mqtt_bridge_destroy(br);
}

TEST_MAIN()
