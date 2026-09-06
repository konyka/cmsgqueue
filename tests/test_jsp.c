/* v0.5.105: $JS hash partitions via append_key. */
#include "cmq_test.h"
#include "cmq_js.h"
#include "cmq_stream.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JDIR "/tmp/cmq_js_parts"

static void wipe(void) {
    remove(JDIR "/js/ev.parts");
    remove(JDIR "/js/ev.msgs");
    remove(JDIR "/js/ev.last");
    remove(JDIR "/js/ev.cursors");
}

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

TEST(jsp, isolate) {
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_set_partitions(j, "ev", 4), 0);
    ASSERT_EQ(cmq_js_partitions(j, "ev"), 4u);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k0, 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k1, 1), 1);
    uint8_t a[16], b[16];
    size_t na = 0, nb = 0;
    ASSERT_EQ(cmq_js_consume_part(j, "$JS.ev.w", 0, a, sizeof(a), &na), 1);
    ASSERT_EQ(cmq_js_consume_part(j, "$JS.ev.w", 1, b, sizeof(b), &nb), 1);
    ASSERT(get_be64(a) != get_be64(b));
    ASSERT_EQ(a[8], k0);
    ASSERT_EQ(b[8], k1);
    cmq_js_destroy(j);
}

TEST(jsp, reopen) {
    mkdir(JDIR, 0755);
    wipe();
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_set_partitions(j, "ev", 4), 0);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k0, 1), 1);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k1, 1), 1);
        cmq_js_destroy(j);
    }
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_partitions(j, "ev"), 4u);
        uint8_t a[16], b[16];
        size_t na = 0, nb = 0;
        ASSERT_EQ(cmq_js_consume_part(j, "$JS.ev.w", 0, a, sizeof(a), &na), 1);
        ASSERT_EQ(cmq_js_consume_part(j, "$JS.ev.w", 1, b, sizeof(b), &nb), 1);
        ASSERT_EQ(a[8], k0);
        ASSERT_EQ(b[8], k1);
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsp, n1_compat) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_partitions(j, "ev"), 1u);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
    ASSERT(cmq_js_set_partitions(j, "ev", 4) != 0);
    ASSERT_EQ(cmq_js_partitions(j, "ev"), 1u);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[8], (uint8_t)'x');
    cmq_js_destroy(j);
}

TEST(jsp, reject) {
    cmq_js_t *j = cmq_js_create();
    ASSERT(cmq_js_set_partitions(NULL, "ev", 4) != 0);
    ASSERT(cmq_js_set_partitions(j, NULL, 4) != 0);
    ASSERT(cmq_js_set_partitions(j, "ev", 0) != 0);
    ASSERT(cmq_js_set_partitions(j, "ev", 17) != 0);
    ASSERT_EQ(cmq_js_partitions(NULL, "ev"), 0u);
    uint8_t out[16];
    size_t n = 0;
    ASSERT(cmq_js_consume_part(j, "$JS.ev.w", 0, out, sizeof(out), &n) != 1);
    cmq_js_destroy(j);
}

TEST_MAIN()
