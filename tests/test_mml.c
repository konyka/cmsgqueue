/* v0.5.156: reload copies mqtt_bridge_maps onto live config. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

static void free_maps(cmq_config_t *c) {
    for (int i = 0; i < c->mqtt_bridge_map_count && i < 8; i++) {
        free((void *)c->mqtt_bridge_maps[i].cmq_subject);
        free((void *)c->mqtt_bridge_maps[i].mqtt_topic);
        c->mqtt_bridge_maps[i].cmq_subject = NULL;
        c->mqtt_bridge_maps[i].mqtt_topic = NULL;
    }
    c->mqtt_bridge_map_count = 0;
}

TEST(mml, apply) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.mqtt_bridge_maps[0].cmq_subject = strdup("old.s");
    live.mqtt_bridge_maps[0].mqtt_topic = strdup("old/t");
    live.mqtt_bridge_map_count = 1;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.mqtt_bridge_maps[0].cmq_subject = "a.s";
    fresh.mqtt_bridge_maps[0].mqtt_topic = "a/t";
    fresh.mqtt_bridge_maps[0].qos = 1;
    fresh.mqtt_bridge_map_count = 1;
    ASSERT_EQ(cmq_reload_apply_mqtt_maps_live(&live, &fresh), 0);
    ASSERT_EQ(live.mqtt_bridge_map_count, 1);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].cmq_subject, "a.s");
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].mqtt_topic, "a/t");
    ASSERT_EQ(live.mqtt_bridge_maps[0].qos, 1);
    ASSERT_EQ(cmq_reload_apply_mqtt_maps_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].cmq_subject, "a.s");
    free_maps(&live);
}

TEST(mml, omitted) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.mqtt_bridge_maps[0].cmq_subject = strdup("keep.s");
    live.mqtt_bridge_maps[0].mqtt_topic = strdup("keep/t");
    live.mqtt_bridge_map_count = 1;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_mqtt_maps_live(&live, &fresh), 0);
    ASSERT_EQ(live.mqtt_bridge_map_count, 1);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].cmq_subject, "keep.s");
    free_maps(&live);
}

TEST(mml, empty) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.mqtt_bridge_maps[0].cmq_subject = strdup("keep.s");
    live.mqtt_bridge_maps[0].mqtt_topic = strdup("keep/t");
    live.mqtt_bridge_map_count = 1;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.mqtt_bridge_maps[0].cmq_subject = "";
    fresh.mqtt_bridge_maps[0].mqtt_topic = "";
    fresh.mqtt_bridge_map_count = 0;
    ASSERT_EQ(cmq_reload_apply_mqtt_maps_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].cmq_subject, "keep.s");
    free_maps(&live);
}

TEST(mml, reject) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.mqtt_bridge_maps[0].cmq_subject = strdup("keep.s");
    live.mqtt_bridge_maps[0].mqtt_topic = strdup("keep/t");
    live.mqtt_bridge_map_count = 1;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.mqtt_bridge_maps[0].cmq_subject = "../evil.s";
    fresh.mqtt_bridge_maps[0].mqtt_topic = "ok/t";
    fresh.mqtt_bridge_map_count = 1;
    ASSERT(cmq_reload_apply_mqtt_maps_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].cmq_subject, "keep.s");
    ASSERT(cmq_reload_apply_mqtt_maps_live(NULL, &fresh) != 0);
    fresh.mqtt_bridge_maps[0].cmq_subject = "ok.s";
    fresh.mqtt_bridge_maps[0].mqtt_topic = "bad\\t";
    ASSERT(cmq_reload_apply_mqtt_maps_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.mqtt_bridge_maps[0].mqtt_topic, "keep/t");
    fresh.mqtt_bridge_maps[0].mqtt_topic = "ok/t";
    fresh.mqtt_bridge_maps[0].qos = 3;
    ASSERT(cmq_reload_apply_mqtt_maps_live(&live, &fresh) != 0);
    ASSERT_EQ(live.mqtt_bridge_maps[0].qos, 0);
    free_maps(&live);
}

TEST_MAIN()
