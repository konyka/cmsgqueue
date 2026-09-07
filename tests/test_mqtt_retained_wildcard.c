/* v0.5.43: MQTT 5.0 retained-message + wildcard subscription
 * end-to-end dispatch test.
 *
 * Publishes a retained message, registers a wildcard subscriber,
 * runs the dispatch helper, and verifies the callback fires for
 * matching subscribers and not for non-matching ones.
 *
 * Not a wire-format test — uses cmq_mqtt_store_retained +
 * cmq_mqtt_record_subscriber + the test-only
 * cmq_mqtt_dispatch_retained. Production wire-format round-trip
 * is v0.6 scope.
 */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <string.h>

/* Forward declarations of v0.5.43 test-only helpers. */
void cmq_mqtt_dispatch_retained(const char *topic,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  void (*cb)(int subscriber_idx,
                                            const char *topic,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            void *user),
                                  void *user);
void cmq_mqtt_subs_reset_test(void);

static int g_last_match_idx;
static const char *g_last_match_topic;
static size_t g_last_match_len;
static void match_cb(int idx, const char *topic, const uint8_t *payload,
                       size_t payload_len, void *user) {
    (void)payload; (void)user;
    g_last_match_idx = idx;
    g_last_match_topic = topic;
    g_last_match_len = payload_len;
}

TEST(mqtt_retained_wildcard, plus_matches_and_delivers) {
    cmq_mqtt_subs_reset_test();
    const uint8_t payload[] = "temp=42.5";
    cmq_mqtt_store_retained("sensors/temp", payload, sizeof(payload) - 1);
    cmq_mqtt_record_subscriber("sensors/+");

    g_last_match_idx = -1;
    g_last_match_topic = NULL;
    g_last_match_len = 0;
    cmq_mqtt_dispatch_retained("sensors/temp", payload,
                                sizeof(payload) - 1, match_cb, NULL);
    /* The cb's third arg is the subscriber's filter, not the
     * published topic. Verify it matches the "sensors/+" filter. */
    ASSERT(g_last_match_idx >= 0);
    ASSERT_EQ(strcmp(g_last_match_topic, "sensors/+"), 0);
    ASSERT_EQ(g_last_match_len, sizeof(payload) - 1);
}

TEST(mqtt_retained_wildcard, hash_matches_and_delivers) {
    cmq_mqtt_subs_reset_test();
    const uint8_t payload[] = "temp=42";
    cmq_mqtt_store_retained("sensors/temp", payload, sizeof(payload) - 1);
    cmq_mqtt_record_subscriber("sensors/#");

    g_last_match_idx = -1;
    g_last_match_topic = NULL;
    g_last_match_len = 0;
    cmq_mqtt_dispatch_retained("sensors/temp", payload,
                                sizeof(payload) - 1, match_cb, NULL);
    ASSERT(g_last_match_idx >= 0);
    ASSERT_EQ(strcmp(g_last_match_topic, "sensors/#"), 0);
}

TEST(mqtt_retained_wildcard, non_matching_topic_does_not_deliver) {
    cmq_mqtt_subs_reset_test();
    const uint8_t payload[] = "x=1";
    cmq_mqtt_store_retained("sensors/temp", payload, sizeof(payload) - 1);
    cmq_mqtt_record_subscriber("events/+");

    g_last_match_idx = -1;
    cmq_mqtt_dispatch_retained("sensors/temp", payload,
                                sizeof(payload) - 1, match_cb, NULL);
    /* No subscriber matched "events/+" against "sensors/temp". */
    ASSERT_EQ(g_last_match_idx, -1);
}

TEST_MAIN()
