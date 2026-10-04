/* v0.5.87: partitioned stream consume cursors. */
#include "cmq_test.h"
#include "cmq_stream.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PDIR "/tmp/cmq_stream_parts"

static void wipe(void) {
    remove(PDIR "/parts.cursors");
    remove(PDIR "/parts.cursors.tmp");
}

static int key_for_part(unsigned part, unsigned n, unsigned char *out) {
    for (unsigned i = 0; i < 256; i++) {
        out[0] = (unsigned char)i;
        if (cmq_stream_partition_of(out, 1, n) == part)
            return 0;
    }
    return -1;
}

TEST(spart, isolate) {
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    ASSERT(k0 != k1);
    cmq_stream_t *st = cmq_stream_create("iso", 32, 0);
    ASSERT_NOT_NULL(st);
    ASSERT_EQ(cmq_stream_partitions(st), 1u);
    ASSERT_EQ(cmq_stream_set_partitions(st, 4), 0);
    ASSERT_EQ(cmq_stream_partitions(st), 4u);
    ASSERT(cmq_stream_append_key(st, &k0, 1, (const uint8_t *)"a", 1) != 0);
    ASSERT(cmq_stream_append_key(st, &k1, 1, (const uint8_t *)"b", 1) != 0);
    ASSERT_EQ(cmq_stream_add_consumer(st, "w"), 0);
    uint64_t n0 = cmq_stream_consumer_next_part(st, "w", 0);
    uint64_t n1 = cmq_stream_consumer_next_part(st, "w", 1);
    ASSERT(n0 != 0 && n1 != 0 && n0 != n1);
    ASSERT_EQ(cmq_stream_consumer_ack_part(st, "w", 0, n0), 0);
    ASSERT_EQ(cmq_stream_consumer_next_part(st, "w", 0), (uint64_t)0);
    ASSERT_EQ(cmq_stream_consumer_next_part(st, "w", 1), n1);
    ASSERT(cmq_stream_consumer_ack_part(st, "w", 1, n0) != 0);
    cmq_stream_destroy(st);
}

TEST(spart, reopen) {
    mkdir(PDIR, 0755);
    wipe();
    unsigned char k0, k1;
    ASSERT_EQ(key_for_part(0, 4, &k0), 0);
    ASSERT_EQ(key_for_part(1, 4, &k1), 0);
    {
        cmq_stream_t *st = cmq_stream_create("parts", 32, 0);
        ASSERT_EQ(cmq_stream_set_partitions(st, 4), 0);
        ASSERT_EQ(cmq_stream_set_cursor_path(st, PDIR), 0);
        uint64_t s0 = cmq_stream_append_key(st, &k0, 1, (const uint8_t *)"a", 1);
        uint64_t s1 = cmq_stream_append_key(st, &k1, 1, (const uint8_t *)"b", 1);
        ASSERT(s0 != 0 && s1 != 0);
        ASSERT_EQ(cmq_stream_add_consumer(st, "w"), 0);
        ASSERT_EQ(cmq_stream_consumer_ack_part(st, "w", 0, s0), 0);
        cmq_stream_destroy(st);
    }
    {
        cmq_stream_t *st = cmq_stream_create("parts", 32, 0);
        ASSERT_EQ(cmq_stream_set_partitions(st, 4), 0);
        ASSERT_EQ(cmq_stream_set_cursor_path(st, PDIR), 0);
        ASSERT_EQ(cmq_stream_consumer_next_part(st, "w", 0), (uint64_t)0);
        uint64_t s0 = cmq_stream_append_key(st, &k0, 1, (const uint8_t *)"a", 1);
        uint64_t s1 = cmq_stream_append_key(st, &k1, 1, (const uint8_t *)"b", 1);
        ASSERT(s0 != 0 && s1 != 0);
        ASSERT_EQ(cmq_stream_consumer_next_part(st, "w", 0), (uint64_t)0);
        ASSERT_EQ(cmq_stream_consumer_next_part(st, "w", 1), s1);
        cmq_stream_destroy(st);
    }
    wipe();
}

TEST(spart, n1_compat) {
    cmq_stream_t *st = cmq_stream_create("one", 32, 0);
    ASSERT_EQ(cmq_stream_set_partitions(st, 1), 0);
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"x", 1), (uint64_t)1);
    ASSERT(cmq_stream_set_partitions(st, 4) != 0);
    ASSERT_EQ(cmq_stream_partitions(st), 1u);
    ASSERT_EQ(cmq_stream_add_consumer(st, "c"), 0);
    ASSERT_EQ(cmq_stream_consumer_next_part(st, "c", 0), (uint64_t)1);
    ASSERT_EQ(cmq_stream_consumer_ack_part(st, "c", 0, 1), 0);
    ASSERT_EQ(cmq_stream_consumer_next(st, "c"), (uint64_t)0);
    cmq_stream_destroy(st);
}

TEST(spart, reject) {
    ASSERT_EQ(cmq_stream_set_partitions(NULL, 4), -1);
    ASSERT_EQ(cmq_stream_partitions(NULL), 0u);
    ASSERT_EQ(cmq_stream_partition_of(NULL, 1, 4), 0u);
    ASSERT_EQ(cmq_stream_partition_of((const uint8_t *)"k", 1, 0), 0u);
    cmq_stream_t *st = cmq_stream_create("rej", 8, 0);
    ASSERT(cmq_stream_set_partitions(st, 0) != 0);
    ASSERT(cmq_stream_set_partitions(st, 17) != 0);
    ASSERT_EQ(cmq_stream_append_key(st, NULL, 1, (const uint8_t *)"a", 1), (uint64_t)0);
    ASSERT_EQ(cmq_stream_consumer_next_part(st, "c", 0), (uint64_t)0);
    ASSERT(cmq_stream_consumer_ack_part(st, "c", 0, 1) != 0);
    ASSERT_EQ(cmq_stream_consumer_ack_part(NULL, "c", 0, 1), -1);
    cmq_stream_destroy(st);
}

TEST_MAIN()
