/* v0.5.103: durable last $JS payload. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JDIR "/tmp/cmq_js_last"

static void wipe(void) {
    remove(JDIR "/js/events.last");
    remove(JDIR "/js/events.last.tmp");
    remove(JDIR "/js/a.last");
    remove(JDIR "/js/b.last");
    remove(JDIR "/js/gone.last");
}

TEST(jsl, reopen_restores_last) {
    mkdir(JDIR, 0755);
    wipe();
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_NOT_NULL(j);
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"v1", 2), 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"v2", 2), 1);
        cmq_js_destroy(j);
    }
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        uint8_t out[8];
        size_t n = 0;
        ASSERT_EQ(cmq_js_request(j, "$JS.events", out, sizeof(out), &n), 1);
        ASSERT_EQ(n, (size_t)2);
        ASSERT(memcmp(out, "v2", 2) == 0);
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsl, no_persist_no_file) {
    mkdir(JDIR, 0755);
    wipe();
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"x", 1), 1);
    cmq_js_destroy(j);
    struct stat s;
    ASSERT(stat(JDIR "/js/events.last", &s) != 0);
}

TEST(jsl, isolated) {
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
        uint8_t out[8];
        size_t n = 0;
        ASSERT_EQ(cmq_js_request(j, "$JS.a", out, sizeof(out), &n), 1);
        ASSERT_EQ(out[0], (uint8_t)'1');
        ASSERT_EQ(cmq_js_request(j, "$JS.b", out, sizeof(out), &n), 1);
        ASSERT_EQ(out[0], (uint8_t)'2');
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsl, reject) {
    cmq_js_t *j = cmq_js_create();
    ASSERT(cmq_js_set_persist(NULL, JDIR) != 0);
    ASSERT(cmq_js_set_persist(j, NULL) != 0);
    ASSERT(cmq_js_set_persist(j, "") != 0);
    ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "$JS.gone", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    cmq_js_destroy(j);
}

TEST_MAIN()
