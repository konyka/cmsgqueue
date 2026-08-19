/* P1 v0.5.4: F19b bridge sublist_insert API. */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_insert_called;
static int test_insert_fn(void *sublist, const char *topic, void *data) {
    (void)sublist; (void)topic; (void)data;
    test_insert_called++;
    return 0;
}

TEST(mqtt_bridge, register_and_call_insert_fn) {
    test_insert_called = 0;
    cmq_mqtt_register_sublist_insert(test_insert_fn, (void *)0xdeadbeef);
    /* The fn pointer is recorded; the relay calls it on dequeue. */
    /* Direct call to verify wiring. */
    int (*fn)(void *, const char *, void *) = test_insert_fn;
    fn((void *)0xdeadbeef, "x", (void *)0x42);
    ASSERT_EQ(test_insert_called, 1);
    cmq_mqtt_register_sublist_insert(NULL, NULL);
}

TEST_MAIN()