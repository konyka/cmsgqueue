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
    ASSERT_EQ(cmq_stream_read(st, 2, &msg), 0);
    ASSERT(memcmp(msg.data, "order2", 6) == 0);

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

TEST(filestore, create_destroy) {
    const char *dir = "/tmp/cmq_fs_test1";
    mkdir(dir, 0755);
    cmq_filestore_t *fs = cmq_filestore_create(dir, "test");
    ASSERT_NOT_NULL(fs);
    cmq_filestore_destroy(fs);
}

TEST(filestore, append_read) {
    const char *dir = "/tmp/cmq_fs_test2";
    mkdir(dir, 0755);
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

    cmq_filestore_destroy(fs);
}

TEST(filestore, persistence) {
    const char *dir = "/tmp/cmq_fs_test3";
    mkdir(dir, 0755);
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
    cmq_filestore_t *fs = cmq_filestore_create(dir, "empty");
    ASSERT_NOT_NULL(fs);
    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT(cmq_filestore_read(fs, 1, &data, &len) != 0);
    cmq_filestore_destroy(fs);
}

TEST_MAIN()
