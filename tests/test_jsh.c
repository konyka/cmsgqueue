/* v0.5.104: durable $JS history so consume survives reopen. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JDIR "/tmp/cmq_js_hist"

static void wipe(void) {
    remove(JDIR "/js/ev.msgs");
    remove(JDIR "/js/ev.msgs.tmp");
    remove(JDIR "/js/ev.last");
    remove(JDIR "/js/ev.cursors");
    remove(JDIR "/js/a.msgs");
    remove(JDIR "/js/b.msgs");
    remove(JDIR "/js/a.last");
    remove(JDIR "/js/b.last");
}

static uint64_t get_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

TEST(jsh, reopen_consume) {
    mkdir(JDIR, 0755);
    wipe();
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_NOT_NULL(j);
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"a", 1), 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"b", 1), 1);
        cmq_js_destroy(j);
    }
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        uint8_t out[16];
        size_t n = 0;
        ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(n, (size_t)9);
        ASSERT_EQ(get_be64(out), (uint64_t)1);
        ASSERT_EQ(out[8], (uint8_t)'a');
        uint8_t ack[8];
        put_be64(ack, 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev.w", ack, 8), 1);
        ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(get_be64(out), (uint64_t)2);
        ASSERT_EQ(out[8], (uint8_t)'b');
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsh, no_persist_no_file) {
    mkdir(JDIR, 0755);
    wipe();
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
    cmq_js_destroy(j);
    struct stat s;
    ASSERT(stat(JDIR "/js/ev.msgs", &s) != 0);
}

TEST(jsh, isolated) {
    mkdir(JDIR, 0755);
    wipe();
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_publish(j, "$JS.a", (const uint8_t *)"1", 1), 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.b", (const uint8_t *)"2", 1), 1);
        cmq_js_destroy(j);
    }
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        uint8_t out[16];
        size_t n = 0;
        ASSERT_EQ(cmq_js_consume(j, "$JS.a.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(out[8], (uint8_t)'1');
        ASSERT_EQ(cmq_js_consume(j, "$JS.b.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(out[8], (uint8_t)'2');
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsh, reject) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.none.w", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    ASSERT(cmq_js_set_persist(NULL, JDIR) != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
