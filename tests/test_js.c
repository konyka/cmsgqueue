/* v0.5.93: $JS.<name> stream PUBLISH path. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <string.h>

TEST(js, parse) {
    char n[CMQ_JS_NAME_MAX];
    ASSERT_EQ(cmq_js_parse("$JS.orders", n, sizeof(n)), 0);
    ASSERT_STR_EQ(n, "orders");
    ASSERT_EQ(cmq_js_parse("foo.bar", n, sizeof(n)), -1);
    ASSERT_EQ(cmq_js_parse("$JS.", n, sizeof(n)), -2);
    ASSERT_EQ(cmq_js_parse("$JS.bad.name", n, sizeof(n)), -2);
}

TEST(js, append_read) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"a", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.events", (const uint8_t *)"b", 1), 1);
    uint8_t out[8];
    size_t n = 0;
    uint64_t seq = 0;
    ASSERT_EQ(cmq_js_last(j, "$JS.events", out, sizeof(out), &n, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)2);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], (uint8_t)'b');
    cmq_js_destroy(j);
}

TEST(js, isolate) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.a", (const uint8_t *)"1", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.b", (const uint8_t *)"2", 1), 1);
    uint8_t out[8];
    size_t n = 0;
    uint64_t seq = 0;
    ASSERT_EQ(cmq_js_last(j, "$JS.a", out, sizeof(out), &n, &seq), 0);
    ASSERT_EQ(out[0], (uint8_t)'1');
    ASSERT_EQ(cmq_js_last(j, "$JS.b", out, sizeof(out), &n, &seq), 0);
    ASSERT_EQ(out[0], (uint8_t)'2');
    cmq_js_destroy(j);
}

TEST(js, reject) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "plain", (const uint8_t *)"x", 1), 0);
    ASSERT(cmq_js_publish(j, "$JS.", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_js_publish(j, "$JS.bad name", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_js_publish(j, "$JS.events", NULL, 1) < 0);
    ASSERT(cmq_js_parse(NULL, NULL, 0) != 0);
    uint8_t out[8];
    size_t n = 0;
    uint64_t seq = 0;
    ASSERT(cmq_js_last(j, "$JS.missing", out, sizeof(out), &n, &seq) != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
