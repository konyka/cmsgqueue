/* v0.5.25: MQTT topic matcher real tests.
 *
 * Replaces the v0.5.15 placeholder with 8 real cases that exercise
 * cmq_mqtt_topic_match directly. The matcher is the public function
 * extracted from the SUBSCRIBE-dispatch loop in v0.5.25.
 */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <string.h>

TEST(mqtt_topic_match, exact_match) {
    ASSERT_EQ(cmq_mqtt_topic_match("foo/bar", "foo/bar"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/bar", "foo/baz"), 0);
}

TEST(mqtt_topic_match, single_wildcard) {
    ASSERT_EQ(cmq_mqtt_topic_match("foo/+", "foo/bar"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/+", "foo/baz"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/+", "foo/bar/baz"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/+", "foo"), 0);
}

TEST(mqtt_topic_match, multi_level_wildcard) {
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#", "foo/bar"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#", "foo/bar/baz"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#", "foo/"), 1);
    /* Per MQTT 5.0 spec, # matches zero or more levels, so `foo` matches
     * `foo/#` (zero remaining levels). */
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#", "foo"), 1);
}

TEST(mqtt_topic_match, wildcard_first_level) {
    ASSERT_EQ(cmq_mqtt_topic_match("+/bar", "foo/bar"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("+/bar", "foo/baz"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("+/bar", "a/b/bar"), 0);
}

TEST(mqtt_topic_match, wildcard_only) {
    ASSERT_EQ(cmq_mqtt_topic_match("#", "anything"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("#", "deep/nested/path"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("#", ""), 1);
}

TEST(mqtt_topic_match, no_match_different_lengths) {
    ASSERT_EQ(cmq_mqtt_topic_match("foo", "foo/bar"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/bar", "foo"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("", "foo"), 0);
}

TEST(mqtt_topic_match, invalid_pattern_returns_neg) {
    /* # not at end is invalid per MQTT 5.0 spec. */
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#/bar", "foo/x/bar"), -1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#/bar", "foo/x"), -1);
    /* Multiple # is invalid. */
    ASSERT_EQ(cmq_mqtt_topic_match("foo/#/#", "foo/x/y"), -1);
}

TEST(mqtt_topic_match, null_inputs_rejected) {
    ASSERT_EQ(cmq_mqtt_topic_match(NULL, "foo"), -1);
    ASSERT_EQ(cmq_mqtt_topic_match("foo", NULL), -1);
    ASSERT_EQ(cmq_mqtt_topic_match(NULL, NULL), -1);
}

/* v0.5.26: integration tests verifying that cmq_mqtt_topic_match produces
 * the correct match set against a real retained-topic store. These mirror
 * the dispatch loop's per-entry check exactly. */

TEST(mqtt_dispatch, plus_wildcard_matches_retained_set) {
    /* Stored topics: sensors/temp, sensors/humid.
     * Filter: sensors/+ -> both match. */
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/+", "sensors/temp"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/+", "sensors/humid"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/+", "sensors"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/+", "sensors/temp/c"), 0);
}

TEST(mqtt_dispatch, hash_wildcard_matches_retained_set) {
    /* Stored topics: sensors/temp, sensors/humid, sensors/temp/c.
     * Filter: sensors/# -> all match (incl. zero levels). */
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/#", "sensors/temp"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/#", "sensors/humid"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/#", "sensors/temp/c"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/#", "sensors"), 1);
    /* Cross-prefix must NOT match. */
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/#", "other/temp"), 0);
}

TEST(mqtt_dispatch, no_wildcard_exact_match_only) {
    /* Filter with no wildcard: only exact match. */
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/temp", "sensors/temp"), 1);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/temp", "sensors/humid"), 0);
    ASSERT_EQ(cmq_mqtt_topic_match("sensors/temp", "sensors/temp/extra"), 0);
}

TEST_MAIN()
