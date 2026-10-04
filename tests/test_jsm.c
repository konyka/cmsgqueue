/* v0.5.108: $JS history WAL rotate so .msgs stays bounded. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JDIR "/tmp/cmq_js_mrot"

static void wipe(void) {
    remove(JDIR "/js/ev.msgs");
    remove(JDIR "/js/ev.msgs.tmp");
    remove(JDIR "/js/ev.last");
    remove(JDIR "/js/ev.cursors");
}

TEST(jsm, rotate_keeps_tail) {
    mkdir(JDIR, 0755);
    wipe();
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 85), 0);
    for (int i = 0; i < 10; i++) {
        uint8_t b = (uint8_t)('0' + i);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &b, 1), 1);
    }
    struct stat st;
    ASSERT_EQ(stat(JDIR "/js/ev.msgs", &st), 0);
    ASSERT_EQ((long)st.st_size, 85L);
    cmq_js_destroy(j);

    j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
    for (int i = 0; i < 5; i++) {
        uint8_t out[16];
        size_t n = 0;
        ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(out[8], (uint8_t)('5' + i));
        uint8_t ack[8] = {0};
        ack[7] = (uint8_t)(i + 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev.w", ack, 8), 1);
    }
    cmq_js_destroy(j);
    wipe();
}

TEST(jsm, rotate_off) {
    mkdir(JDIR, 0755);
    wipe();
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 0), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"a", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"b", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"c", 1), 1);
    struct stat st;
    ASSERT_EQ(stat(JDIR "/js/ev.msgs", &st), 0);
    ASSERT_EQ((long)st.st_size, 51L);
    cmq_js_destroy(j);
    wipe();
}

TEST(jsm, no_persist) {
    mkdir(JDIR, 0755);
    wipe();
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 85), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
    struct stat st;
    ASSERT(stat(JDIR "/js/ev.msgs", &st) != 0);
    cmq_js_destroy(j);
}

TEST(jsm, reject) {
    ASSERT(cmq_js_set_msgs_rotate_bytes(NULL, 85) != 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_msgs_rotate_bytes(j, 85), 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
