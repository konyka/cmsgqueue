/* v0.5.42: smoke test for the MQTT QoS2 retransmit table.
 *
 * The two helpers qos2_record_or_lookup and qos2_get_phase are
 * static in cmq_mqtt_server.c. v0.5.42 exposes test-only wrappers
 * (cmq_mqtt_qos2_record_or_lookup_test / _get_phase_test) so the
 * table-management logic can be unit-tested without driving a real
 * MQTT wire-format client. v0.5.42 also fixes qos2_record_or_lookup
 * to return -1 on table overflow (previously silently dropped).
 *
 * This is NOT a wire-format test. A full MQTT PUBLISH/PUBREC/PUBREL
 * round-trip is v0.6 scope.
 *
 * Test packet_ids are offset by the test index to avoid collisions
 * across tests (the table is global state with no reset between
 * tests in this framework).
 */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>

/* Forward declarations of v0.5.42 test-only helpers. */
int cmq_mqtt_qos2_record_or_lookup_test(uint16_t packet_id, int new_phase);
int cmq_mqtt_qos2_get_phase_test(uint16_t packet_id);
void cmq_mqtt_qos2_reset_test(void);

TEST(mqtt_qos2, empty_table_returns_phase_zero) {
    /* A packet_id that has not been recorded should yield phase 0
     * (lookup miss). Reset first to clear any state from previous
     * tests. */
    cmq_mqtt_qos2_reset_test();
    ASSERT_EQ(cmq_mqtt_qos2_get_phase_test(42), 0);
}

TEST(mqtt_qos2, insert_new_returns_zero_and_sets_phase) {
    cmq_mqtt_qos2_reset_test();
    /* First insert returns 0 (newly added), and a subsequent get
     * returns the recorded phase. */
    int rc = cmq_mqtt_qos2_record_or_lookup_test(42, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_mqtt_qos2_get_phase_test(42), 1);
}

TEST(mqtt_qos2, update_existing_returns_one) {
    cmq_mqtt_qos2_reset_test();
    /* A second call with the same packet_id returns 1 (entry
     * existed) and updates the phase. */
    ASSERT_EQ(cmq_mqtt_qos2_record_or_lookup_test(43, 1), 0);
    int rc = cmq_mqtt_qos2_record_or_lookup_test(43, 2);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(cmq_mqtt_qos2_get_phase_test(43), 2);
}

TEST(mqtt_qos2, distinct_packet_ids_isolated) {
    cmq_mqtt_qos2_reset_test();
    /* Different packet_ids are tracked independently. */
    cmq_mqtt_qos2_record_or_lookup_test(100, 1);
    cmq_mqtt_qos2_record_or_lookup_test(101, 2);
    ASSERT_EQ(cmq_mqtt_qos2_get_phase_test(100), 1);
    ASSERT_EQ(cmq_mqtt_qos2_get_phase_test(101), 2);
}

TEST(mqtt_qos2, table_overflow_returns_neg_one) {
    cmq_mqtt_qos2_reset_test();
    /* The table caps at MQTT_QOS2_MAX (128). After filling all
     * slots, the next insert returns -1. Existing entries remain
     * queryable. */
    uint16_t base = 5000;
    int first_overflow = -1;
    for (int i = 0; i < 200; i++) {
        int rc = cmq_mqtt_qos2_record_or_lookup_test((uint16_t)(base + i),
                                                    1);
        if (rc == -1) {
            first_overflow = i;
            break;
        }
    }
    /* Must have hit overflow between 0 and 199. */
    ASSERT(first_overflow >= 0);
    ASSERT(first_overflow < 200);
    /* Spot-check that an early entry (in the table, not the failed
     * one) is still queryable. */
    int ph = cmq_mqtt_qos2_get_phase_test(base);
    ASSERT(ph == 1);
}

TEST_MAIN()

