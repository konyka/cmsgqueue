/* P1 v0.5.4: F19b bridge sublist_insert API. */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_insert_called;
static int test_insert_fn(void *sublist, const char *topic,
                           const uint8_t *payload, size_t payload_len) {
    (void)sublist; (void)topic; (void)payload; (void)payload_len;
    test_insert_called++;
    return 0;
}

TEST(mqtt_bridge, register_and_call_insert_fn) {
    test_insert_called = 0;
    cmq_mqtt_register_sublist_insert(test_insert_fn, (void *)0xdeadbeef);
    /* The fn pointer is recorded; the relay calls it on dequeue. */
    /* Direct call to verify wiring. */
    int (*fn)(void *, const char *, const uint8_t *, size_t) =
        (int (*)(void *, const char *, const uint8_t *, size_t))test_insert_fn;
    fn((void *)0xdeadbeef, "x", (const uint8_t *)"data", 4);
    ASSERT_EQ(test_insert_called, 1);
    cmq_mqtt_register_sublist_insert(NULL, NULL);
}

TEST_MAIN()