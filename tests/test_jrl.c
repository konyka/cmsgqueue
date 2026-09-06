/* v0.5.130: reload applies js_partitions / js_msgs_rotate_bytes. */
#include "cmq_js.h"
#include "cmq_test.h"
#include <string.h>

TEST(jrl, apply) {
    cmq_js_t *j = cmq_js_create();
    ASSERT(j != NULL);
    ASSERT_EQ(cmq_js_set_default_partitions(j, 2), 0);
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 100), 0);
    int parts = 2, rotate = 100;
    ASSERT_EQ(cmq_js_reload(j, &parts, &rotate, 4, 200), 0);
    ASSERT_EQ(parts, 4);
    ASSERT_EQ(rotate, 200);
    ASSERT_EQ(cmq_js_default_partitions(j), 4u);
    ASSERT_EQ(cmq_js_msgs_rotate_bytes(j), 200ull);
    cmq_js_destroy(j);
}

TEST(jrl, omitted) {
    int parts = 3, rotate = 50;
    ASSERT_EQ(cmq_js_reload(NULL, &parts, &rotate, 0, 0), 0);
    ASSERT_EQ(parts, 3);
    ASSERT_EQ(rotate, 50);
}

TEST(jrl, empty) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_default_partitions(j, 2), 0);
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 80), 0);
    int parts = 2, rotate = 80;
    ASSERT_EQ(cmq_js_reload(j, &parts, &rotate, 0, 0), 0);
    ASSERT_EQ(parts, 2);
    ASSERT_EQ(rotate, 80);
    ASSERT_EQ(cmq_js_default_partitions(j), 2u);
    ASSERT_EQ(cmq_js_msgs_rotate_bytes(j), 80ull);
    cmq_js_destroy(j);
}

TEST(jrl, reject) {
    int parts = 2, rotate = 40;
    ASSERT(cmq_js_reload(NULL, &parts, &rotate, 17, 0) != 0);
    ASSERT_EQ(parts, 2);
    ASSERT_EQ(rotate, 40);
    ASSERT(cmq_js_reload(NULL, &parts, &rotate, 0, 1073741825) != 0);
    ASSERT_EQ(parts, 2);
    ASSERT(cmq_js_reload(NULL, NULL, &rotate, 4, 0) != 0);
    ASSERT(cmq_js_reload(NULL, &parts, NULL, 4, 0) != 0);
}

TEST_MAIN()
