/* v0.5.107: default $JS partitions for new streams. */
#include "cmq_test.h"
#include "cmq_js.h"
#include "cmq_stream.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JDIR "/tmp/cmq_js_defp"

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

TEST(jsd, apply) {
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_set_default_partitions(j, 4), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k0, 1), 1);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", &k1, 1), 1);
    ASSERT_EQ(cmq_js_partitions(j, "ev"), 4u);
    uint8_t a[16], b[16];
    size_t na = 0, nb = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.0", a, sizeof(a), &na), 1);
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w.1", b, sizeof(b), &nb), 1);
    ASSERT_EQ(a[8], k0);
    ASSERT_EQ(b[8], k1);
    cmq_js_destroy(j);
}

TEST(jsd, file_wins) {
    mkdir(JDIR, 0755);
    wipe();
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        ASSERT_EQ(cmq_js_set_partitions(j, "ev", 2), 0);
        ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
        cmq_js_destroy(j);
    }
    {
        cmq_js_t *j = cmq_js_create();
        ASSERT_EQ(cmq_js_set_default_partitions(j, 4), 0);
        ASSERT_EQ(cmq_js_set_persist(j, JDIR), 0);
        uint8_t out[16];
        size_t n = 0;
        ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
        ASSERT_EQ(cmq_js_partitions(j, "ev"), 2u);
        cmq_js_destroy(j);
    }
    wipe();
}

TEST(jsd, n1) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_EQ(cmq_js_set_default_partitions(j, 1), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"x", 1), 1);
    ASSERT_EQ(cmq_js_partitions(j, "ev"), 1u);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_js_consume(j, "$JS.ev.w", out, sizeof(out), &n), 1);
    ASSERT_EQ(out[8], (uint8_t)'x');
    cmq_js_destroy(j);
}

TEST(jsd, reject) {
    ASSERT(cmq_js_set_default_partitions(NULL, 4) != 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT(cmq_js_set_default_partitions(j, 0) != 0);
    ASSERT(cmq_js_set_default_partitions(j, 17) != 0);
    ASSERT_EQ(cmq_js_set_default_partitions(j, 4), 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
