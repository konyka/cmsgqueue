/* v0.5.106: REQUEST $JS.name.cons.part → consume_part. */
#include "cmq_test.h"
#include "cmq_js.h"
#include "cmq_stream.h"
#include <string.h>

static int key_for_part(unsigned part, unsigned n, unsigned char *out) {
    for (unsigned i = 0; i < 256; i++) {
        out[0] = (unsigned char)i;
        if (cmq_stream_partition_of(out, 1, n) == part)
            return 0;
    }
    return -1;
}

static uint64_t get_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

TEST(jsq, parse) {
    char n[CMQ_JS_NAME_MAX], c[CMQ_JS_NAME_MAX];
    unsigned p = 99;
    ASSERT_EQ(cmq_js_parse_part("$JS.ev.w.0", n, sizeof(n), c, sizeof(c), &p), 0);
    ASSERT_STR_EQ(n, "ev");
    ASSERT_STR_EQ(c, "w");
    ASSERT_EQ(p, 0u);
    ASSERT_EQ(cmq_js_parse_part("$JS.ev.w.15", n, sizeof(n), c, sizeof(c), &p), 0);
    ASSERT_EQ(p, 15u);
    ASSERT_EQ(cmq_js_parse_part("$JS.ev", n, sizeof(n), c, sizeof(c), &p), -1);
    ASSERT_EQ(cmq_js_parse_part("$JS.ev.w", n, sizeof(n), c, sizeof(c), &p), -2);
    ASSERT_EQ(cmq_js_parse_part("plain", n, sizeof(n), c, sizeof(c), &p), -1);
}

TEST(jsq, isolate) {
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_partitions(j, "ev", 4), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k0, 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k1, 1), 1);
    uint8_t a[16], b[16];
    size_t na = 0, nb = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.0", a, sizeof(a), &na), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.1", b, sizeof(b), &nb), 1);
    ASSERT(get_be64(a) != get_be64(b));
    ASSERT_EQ(a[8], k0);
    ASSERT_EQ(b[8], k1);
    cmq_js_destroy(j);
}

TEST(jsq, compat) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[8], (uint8_t)'x');
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.0", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[8], (uint8_t)'x');
    cmq_js_destroy(j);
}

TEST(jsq, reject) {
    char n[CMQ_JS_NAME_MAX], c[CMQ_JS_NAME_MAX];
    unsigned p = 0;
    ASSERT(cmq_js_parse_part("$JS.ev.w.16", n, sizeof(n), c, sizeof(c), &p) != 0);
    ASSERT(cmq_js_parse_part("$JS.ev.w.x", n, sizeof(n), c, sizeof(c), &p) != 0);
    ASSERT(cmq_js_parse_part("$JS.ev.w.0.1", n, sizeof(n), c, sizeof(c), &p) != 0);
    ASSERT(cmq_js_parse_cons("$JS.a.b.c", n, sizeof(n), c, sizeof(c)) != 0);
    cmq_js_t *j = cmq_js_create();
    uint8_t out[16];
    size_t ln = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.16", out, sizeof(out), &ln), -1);
    ASSERT(cmq_js_parse_part(NULL, n, sizeof(n), c, sizeof(c), &p) != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
