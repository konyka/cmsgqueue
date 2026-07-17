#include "cmq_store.h"
#include "cmq_stream.h"
#include "cmq_filestore.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

TEST(store, create_destroy) {
    cmq_store_t *s = cmq_store_create(64);
    ASSERT_NOT_NULL(s);
    cmq_store_destroy(s);
}

TEST(store, put_get) {
    cmq_store_t *s = cmq_store_create(64);
    uint64_t seq = cmq_store_put(s, (const uint8_t *)"hello", 5);
    ASSERT_EQ(seq, (uint64_t)1);
    ASSERT_EQ(cmq_store_count(s), (size_t)1);

    cmq_store_msg_t msg;
    ASSERT_EQ(cmq_store_get(s, 1, &msg), 0);
    ASSERT_EQ(msg.seq, (uint64_t)1);
    ASSERT_EQ(msg.len, (size_t)5);
    ASSERT(memcmp(msg.data, "hello", 5) == 0);
    ASSERT(msg.timestamp_ms > 0);
    cmq_store_msg_release(&msg);

    cmq_store_destroy(s);
}

TEST(store, put_rejects_oversize) {
    cmq_store_t *s = cmq_store_create(8);
    size_t huge = 16u * 1024 * 1024 + 1;
    uint8_t *buf = malloc(huge);
    ASSERT_NOT_NULL(buf);
    memset(buf, 1, huge);
    ASSERT_EQ(cmq_store_put(s, buf, huge), (uint64_t)0);
    ASSERT_EQ(cmq_store_count(s), (size_t)0);
    free(buf);
    cmq_store_destroy(s);
}

TEST(store, ring_overwrite) {
    cmq_store_t *s = cmq_store_create(4);
    for (int i = 0; i < 6; i++) {
        uint8_t buf[8];
        buf[0] = (uint8_t)i;
        cmq_store_put(s, buf, 1);
    }
    ASSERT_EQ(cmq_store_count(s), (size_t)4);
    ASSERT_EQ(cmq_store_first_seq(s), (uint64_t)3);
    ASSERT_EQ(cmq_store_last_seq(s), (uint64_t)6);

    cmq_store_msg_t msg;
    ASSERT(cmq_store_get(s, 2, &msg) != 0);
    ASSERT_EQ(cmq_store_get(s, 3, &msg), 0);
    ASSERT_EQ(msg.seq, (uint64_t)3);
    cmq_store_msg_release(&msg);

    cmq_store_destroy(s);
}

TEST(store, truncate) {
    cmq_store_t *s = cmq_store_create(16);
    for (int i = 0; i < 5; i++) {
        cmq_store_put(s, (const uint8_t *)"x", 1);
    }
    cmq_store_truncate(s, 3);
    ASSERT_EQ(cmq_store_count(s), (size_t)3);
    ASSERT_EQ(cmq_store_first_seq(s), (uint64_t)3);

    cmq_store_destroy(s);
}

TEST(store, evict_prefix_only) {
    cmq_store_t *s = cmq_store_create(8);
    for (int i = 0; i < 4; i++)
        cmq_store_put(s, (const uint8_t *)"x", 1);
    ASSERT_EQ(cmq_store_first_seq(s), (uint64_t)1);
    ASSERT_EQ(cmq_store_evict_seq(s, 2), -1); /* hole not allowed */
    ASSERT_EQ(cmq_store_evict_seq(s, 1), 0);
    ASSERT_EQ(cmq_store_first_seq(s), (uint64_t)2);
    ASSERT_EQ(cmq_store_count(s), (size_t)3);
    cmq_store_destroy(s);
}

TEST(store, empty_get) {
    cmq_store_t *s = cmq_store_create(16);
    cmq_store_msg_t msg;
    ASSERT(cmq_store_get(s, 1, &msg) != 0);
    cmq_store_destroy(s);
}

TEST(stream, create_destroy) {
    cmq_stream_t *st = cmq_stream_create("events", 100, 0);
    ASSERT_NOT_NULL(st);
    ASSERT_STR_EQ(cmq_stream_name(st), "events");
    cmq_stream_destroy(st);
}

TEST(stream, append_read) {
    cmq_stream_t *st = cmq_stream_create("orders", 100, 0);
    uint64_t s1 = cmq_stream_append(st, (const uint8_t *)"order1", 6);
    uint64_t s2 = cmq_stream_append(st, (const uint8_t *)"order2", 6);
    ASSERT_EQ(s1, (uint64_t)1);
    ASSERT_EQ(s2, (uint64_t)2);
    ASSERT_EQ(cmq_stream_msg_count(st), (size_t)2);

    cmq_stream_msg_t msg;
    ASSERT_EQ(cmq_stream_read(st, 1, &msg), 0);
    ASSERT(memcmp(msg.data, "order1", 6) == 0);
    cmq_stream_msg_release(&msg);
    ASSERT_EQ(cmq_stream_read(st, 2, &msg), 0);
    ASSERT(memcmp(msg.data, "order2", 6) == 0);
    cmq_stream_msg_release(&msg);

    cmq_stream_destroy(st);
}

TEST(stream, consumer_ack) {
    cmq_stream_t *st = cmq_stream_create("tasks", 100, 0);
    cmq_stream_append(st, (const uint8_t *)"t1", 2);
    cmq_stream_append(st, (const uint8_t *)"t2", 2);
    cmq_stream_append(st, (const uint8_t *)"t3", 2);

    ASSERT_EQ(cmq_stream_add_consumer(st, "worker1"), 0);
    ASSERT_EQ(cmq_stream_add_consumer(st, "worker1"), 0);

    cmq_stream_consumer_t state = cmq_stream_consumer_state(st, "worker1");
    ASSERT_EQ(state.consumer_seq, (uint64_t)0);
    ASSERT_EQ(state.pending_count, (uint32_t)3);

    ASSERT_EQ(cmq_stream_consumer_ack(st, "worker1", 2), 0);
    state = cmq_stream_consumer_state(st, "worker1");
    ASSERT_EQ(state.consumer_seq, (uint64_t)2);
    ASSERT_EQ(state.pending_count, (uint32_t)1);

    uint64_t next = cmq_stream_consumer_next(st, "worker1");
    ASSERT_EQ(next, (uint64_t)3);

    cmq_stream_destroy(st);
}

TEST(stream, max_bytes_eviction) {
    cmq_stream_t *st = cmq_stream_create("limited", 100, 10);
    cmq_stream_append(st, (const uint8_t *)"12345", 5);
    cmq_stream_append(st, (const uint8_t *)"67890", 5);
    ASSERT_EQ(cmq_stream_msg_count(st), (size_t)2);

    cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8);
    ASSERT_EQ(cmq_stream_msg_count(st), (size_t)1);
    ASSERT_EQ(cmq_stream_first_seq(st), (uint64_t)3);

    cmq_stream_destroy(st);
}

/* Late join after eviction must start at first retained seq, not seq 1. */
TEST(stream, late_consumer_skips_evicted) {
    cmq_stream_t *st = cmq_stream_create("late", 100, 10);
    cmq_stream_append(st, (const uint8_t *)"12345", 5);
    cmq_stream_append(st, (const uint8_t *)"67890", 5);
    cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8);
    ASSERT_EQ(cmq_stream_first_seq(st), (uint64_t)3);
    ASSERT_EQ(cmq_stream_add_consumer(st, "w"), 0);
    ASSERT_EQ(cmq_stream_consumer_next(st, "w"), (uint64_t)3);
    cmq_stream_consumer_t state = cmq_stream_consumer_state(st, "w");
    ASSERT_EQ(state.pending_count, (uint32_t)1);
    cmq_stream_msg_t msg;
    ASSERT_EQ(cmq_stream_read(st, 3, &msg), 0);
    cmq_stream_msg_release(&msg);
    cmq_stream_destroy(st);
}

TEST(stream, retain_unacked_on_pressure) {
    cmq_stream_t *st = cmq_stream_create("retain", 100, 10);
    ASSERT_EQ(cmq_stream_add_consumer(st, "slow"), 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"12345", 5) > 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"67890", 5) > 0);
    /* Consumer has not acked — must refuse rather than drop pending. */
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8), (uint64_t)0);
    ASSERT_EQ(cmq_stream_msg_count(st), (size_t)2);
    ASSERT_EQ(cmq_stream_first_seq(st), (uint64_t)1);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "slow", 2), 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8) > 0);
    ASSERT_EQ(cmq_stream_first_seq(st), (uint64_t)3);
    cmq_stream_destroy(st);
}

TEST(stream, remove_consumer_unpins_retention) {
    cmq_stream_t *st = cmq_stream_create("unpin", 100, 10);
    ASSERT_EQ(cmq_stream_add_consumer(st, "zombie"), 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"12345", 5) > 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"67890", 5) > 0);
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8), (uint64_t)0);
    ASSERT_EQ(cmq_stream_remove_consumer(st, "zombie"), 0);
    ASSERT_EQ(cmq_stream_remove_consumer(st, "zombie"), -1);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8) > 0);
    cmq_stream_destroy(st);
}

TEST(stream, ack_rejects_beyond_last) {
    cmq_stream_t *st = cmq_stream_create("ackbound", 100, 10);
    ASSERT_EQ(cmq_stream_add_consumer(st, "c"), 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"12345", 5) > 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"67890", 5) > 0);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "c", 999), -1);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "c", 0), -1);
    /* Rejected over-ack must not raise retain_floor — still refuse under pressure. */
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8), (uint64_t)0);
    ASSERT_EQ(cmq_stream_msg_count(st), (size_t)2);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "c", 2), 0);
    ASSERT(cmq_stream_append(st, (const uint8_t *)"abcdefgh", 8) > 0);
    cmq_stream_destroy(st);
}

TEST(filestore, create_destroy) {
    const char *dir = "/tmp/cmq_fs_test1";
    mkdir(dir, 0755);
    remove("/tmp/cmq_fs_test1/test.data");
    remove("/tmp/cmq_fs_test1/test.idx");
    cmq_filestore_t *fs = cmq_filestore_create(dir, "test");
    ASSERT_NOT_NULL(fs);
    cmq_filestore_destroy(fs);
}

TEST(filestore, append_read) {
    const char *dir = "/tmp/cmq_fs_test2";
    mkdir(dir, 0755);
    remove("/tmp/cmq_fs_test2/rw.data");
    remove("/tmp/cmq_fs_test2/rw.idx");
    cmq_filestore_t *fs = cmq_filestore_create(dir, "rw");
    ASSERT_NOT_NULL(fs);

    uint64_t seq1 = 0, seq2 = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"hello", 5, &seq1), 0);
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"world", 5, &seq2), 0);
    ASSERT_EQ(seq1, (uint64_t)1);
    ASSERT_EQ(seq2, (uint64_t)2);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)2);

    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
    ASSERT_EQ(len, (size_t)5);
    ASSERT(memcmp(data, "hello", 5) == 0);
    free(data);

    ASSERT_EQ(cmq_filestore_read(fs, 2, &data, &len), 0);
    ASSERT(memcmp(data, "world", 5) == 0);
    free(data);

    /* read() leaves FILE mid-stream — append must still go to EOF. */
    uint64_t seq3 = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"again", 5, &seq3), 0);
    ASSERT_EQ(seq3, (uint64_t)3);
    ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
    ASSERT(memcmp(data, "hello", 5) == 0);
    free(data);
    ASSERT_EQ(cmq_filestore_read(fs, 3, &data, &len), 0);
    ASSERT(memcmp(data, "again", 5) == 0);
    free(data);

    cmq_filestore_destroy(fs);
}

TEST(filestore, persistence) {
    const char *dir = "/tmp/cmq_fs_test3";
    mkdir(dir, 0755);
    remove("/tmp/cmq_fs_test3/persist.data");
    remove("/tmp/cmq_fs_test3/persist.idx");
    {
        cmq_filestore_t *fs = cmq_filestore_create(dir, "persist");
        ASSERT_NOT_NULL(fs);
        cmq_filestore_append(fs, (const uint8_t *)"data1", 5, NULL);
        cmq_filestore_append(fs, (const uint8_t *)"data2", 5, NULL);
        cmq_filestore_sync(fs);
        cmq_filestore_destroy(fs);
    }
    {
        cmq_filestore_t *fs = cmq_filestore_create(dir, "persist");
        ASSERT_NOT_NULL(fs);
        ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)2);
        uint8_t *data = NULL;
        size_t len = 0;
        ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
        ASSERT(memcmp(data, "data1", 5) == 0);
        free(data);
        cmq_filestore_destroy(fs);
    }
}

TEST(filestore, read_nonexistent) {
    const char *dir = "/tmp/cmq_fs_test4";
    mkdir(dir, 0755);
    remove("/tmp/cmq_fs_test4/empty.data");
    remove("/tmp/cmq_fs_test4/empty.idx");
    cmq_filestore_t *fs = cmq_filestore_create(dir, "empty");
    ASSERT_NOT_NULL(fs);
    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT(cmq_filestore_read(fs, 1, &data, &len) != 0);
    cmq_filestore_destroy(fs);
}

TEST(filestore, reject_path_traversal_prefix) {
    const char *dir = "/tmp/cmq_fs_safe";
    mkdir(dir, 0755);
    ASSERT_NULL(cmq_filestore_create(dir, "../escape"));
    ASSERT_NULL(cmq_filestore_create(dir, "a/b"));
    ASSERT_NULL(cmq_filestore_create(dir, ".."));
    ASSERT_NULL(cmq_filestore_create(dir, ""));
    cmq_filestore_t *fs = cmq_filestore_create(dir, "ok_name");
    ASSERT_NOT_NULL(fs);
    cmq_filestore_destroy(fs);
    remove("/tmp/cmq_fs_safe/ok_name.data");
    remove("/tmp/cmq_fs_safe/ok_name.idx");
}

TEST(filestore, reject_path_traversal_dir) {
    ASSERT_NULL(cmq_filestore_create("/tmp/cmq_fs_safe/../escape", "ok"));
    ASSERT_NULL(cmq_filestore_create("..", "ok"));
    ASSERT_NULL(cmq_filestore_create("/tmp/./cmq", "ok"));
    ASSERT_NULL(cmq_filestore_create("", "ok"));
    const char *dir = "/tmp/cmq_fs_safe_dir";
    mkdir(dir, 0755);
    cmq_filestore_t *fs = cmq_filestore_create(dir, "ok_name");
    ASSERT_NOT_NULL(fs);
    cmq_filestore_destroy(fs);
    remove("/tmp/cmq_fs_safe_dir/ok_name.data");
    remove("/tmp/cmq_fs_safe_dir/ok_name.idx");
}

TEST_MAIN()
