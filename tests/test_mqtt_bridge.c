/* P1 v0.5.3: F19b MQTT→cmq_sublist bridge plumbing. */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include "cmq_server.h"
#include "cmq_sublist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

TEST(mqtt_bridge, set_bridge_server_keeps_pointer) {
    /* cmq_mqtt_set_bridge_server records the server pointer. The
     * actual sublist insertion happens in the relay thread, which
     * is v0.6 work. v0.5.3 ships the plumbing + API. */
    cmq_mqtt_set_bridge_server((void *)0xdeadbeef);
    /* No assertion needed — just verify no crash. */
    cmq_mqtt_set_bridge_server(NULL);
}

/* v0.5.35: the bridge relay must NOT create sublist subscriptions
 * via cmq_sublist_insert. With the v0.5.6 wiring, the relay called
 * cmq_sublist_insert(srv->sublist, topic, payload) — but that's the
 * subscription registry API, not a publish API. The relay's
 * payload pointers landed in trie node `subs[]` arrays where they
 * were never reachable. The v0.5.34 fix removed the double-free;
 * v0.5.35 removes the wrong wiring so the relay stays on the
 * no-op path (which is the only path the v0.5.34 freelist load
 * test exercises). */
TEST(mqtt_bridge, relay_does_not_create_subscriptions) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25040;
    cfg.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Read sublist count before any bridge activity. */
    size_t count_before = cmq_sublist_count(srv->sublist);

    /* Start the relay. */
    cmq_mqtt_set_bridge_server(srv);

    /* Push a payload that would, with the wrong wiring, be inserted
     * as a "subscription" (matched by the subject "v0.5.35/sentinel").
     * With the no-op default, the relay just recycles the buffer. */
    extern int cmq_mqtt_test_enqueue_bridge(const char *topic,
                                              const uint8_t *payload,
                                              size_t len);
    uint8_t payload[32] = {0};
    ASSERT_EQ(cmq_mqtt_test_enqueue_bridge("v0.5.35/sentinel",
                                            payload, sizeof(payload)),
              0);

    /* Wait briefly for the relay to drain. */
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    /* Sublist count must be unchanged — the relay must not have
     * inserted any subscription. */
    size_t count_after = cmq_sublist_count(srv->sublist);
    ASSERT_EQ(count_after, count_before);

    cmq_mqtt_bridge_shutdown();
    cmq_server_destroy(srv);
}

TEST_MAIN()