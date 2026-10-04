/* v0.5.94: D4 REQUEST-get for $JS.<name>. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <string.h>

TEST(jsr, hit_miss) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"z", 1), 1);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "$JS.events", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], (uint8_t)'z');
    ASSERT_EQ(cmq_js_request(j, "$JS.missing", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    cmq_js_destroy(j);
}

TEST(jsr, not_js) {
    cmq_js_t *j = cmq_js_create();
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "plain", out, sizeof(out), &n), -1);
    ASSERT_EQ(cmq_js_request(j, "$KV.acc.k", out, sizeof(out), &n), -1);
    cmq_js_destroy(j);
}

TEST(jsr, isolate) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.a", (const uint8_t *)"1", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.b", (const uint8_t *)"2", 1), 1);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "$JS.a", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[0], (uint8_t)'1');
    ASSERT_EQ(cmq_js_request(j, "$JS.b", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[0], (uint8_t)'2');
    cmq_js_destroy(j);
}

TEST(jsr, reject) {
    cmq_js_t *j = cmq_js_create();
    uint8_t out[8];
    size_t n = 0;
    ASSERT(cmq_js_request(NULL, "$JS.a", out, sizeof(out), &n) != 0);
    ASSERT(cmq_js_request(j, NULL, out, sizeof(out), &n) != 0);
    ASSERT(cmq_js_request(j, "$JS.", out, sizeof(out), &n) != 0);
    ASSERT(cmq_js_request(j, "$JS.bad.name", out, sizeof(out), &n) != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
