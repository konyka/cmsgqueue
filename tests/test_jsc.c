/* v0.5.95: $JS.<name>.<consumer> pull consume / ack. */
#include "cmq_test.h"
#include "cmq_js.h"
#include <string.h>

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t get_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

TEST(jsc, consume_ack) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"a", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"b", 1), 1);
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
    ASSERT_EQ(cmq_js_request(j, "$JS.ev", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], (uint8_t)'b');
    cmq_js_destroy(j);
}

TEST(jsc, isolate) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.s", (const uint8_t *)"x", 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.s", (const uint8_t *)"y", 1), 1);
    uint8_t a[16], b[16];
    size_t na = 0, nb = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.s.w1", a, sizeof(a), &na), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.s.w2", b, sizeof(b), &nb), 1);
    ASSERT_EQ(get_be64(a), (uint64_t)1);
    ASSERT_EQ(get_be64(b), (uint64_t)1);
    uint8_t ack[8];
    put_be64(ack, 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.s.w1", ack, 8), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.s.w1", a, sizeof(a), &na), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.s.w2", b, sizeof(b), &nb), 1);
    ASSERT_EQ(get_be64(a), (uint64_t)2);
    ASSERT_EQ(get_be64(b), (uint64_t)1);
    cmq_js_destroy(j);
}

TEST(jsc, miss) {
    cmq_js_t *j = cmq_js_create();
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.none.w", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"a", 1), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
    uint8_t ack[8];
    put_be64(ack, 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev.w", ack, 8), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    cmq_js_destroy(j);
}

TEST(jsc, reject) {
    cmq_js_t *j = cmq_js_create();
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "plain", out, sizeof(out), &n), -1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev", out, sizeof(out), &n), -1);
    ASSERT(cmq_js_consume(NULL, "$JS.ev.w", out, sizeof(out), &n) != 0);
    ASSERT(cmq_js_publish(j, "$JS.ev.w", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_js_parse_cons(NULL, NULL, 0, NULL, 0) != 0);
    char nm[CMQ_JS_NAME_MAX], cn[CMQ_JS_NAME_MAX];
    ASSERT(cmq_js_parse_cons("$JS.a.b.c", nm, sizeof(nm), cn, sizeof(cn))
           != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
