/* v0.5.56: durable stream consumer cursors. */
#include "cmq_test.h"
#include "cmq_stream.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CDIR "/tmp/cmq_stream_cursors"

static void wipe(void) {
    remove(CDIR "/tasks.cursors");
    remove(CDIR "/tasks.cursors.tmp");
    remove(CDIR "/iso.cursors");
    remove(CDIR "/iso.cursors.tmp");
    remove(CDIR "/gone.cursors");
    remove(CDIR "/nopath.cursors");
}

TEST(cursors, no_path_no_file) {
    mkdir(CDIR, 0755);
    wipe();
    cmq_stream_t *st = cmq_stream_create("nopath", 32, 0);
    ASSERT_NOT_NULL(st);
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"a", 1), (uint64_t)1);
    ASSERT_EQ(cmq_stream_add_consumer(st, "c"), 0);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "c", 1), 0);
    cmq_stream_destroy(st);
    struct stat s;
    ASSERT(stat(CDIR "/nopath.cursors", &s) != 0);
}

TEST(cursors, reopen_restores_ack) {
    mkdir(CDIR, 0755);
    wipe();
    {
        cmq_stream_t *st = cmq_stream_create("tasks", 32, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"t1", 2), (uint64_t)1);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"t2", 2), (uint64_t)2);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"t3", 2), (uint64_t)3);
        ASSERT_EQ(cmq_stream_add_consumer(st, "worker1"), 0);
        ASSERT_EQ(cmq_stream_consumer_ack(st, "worker1", 2), 0);
        cmq_stream_destroy(st);
    }
    {
        cmq_stream_t *st = cmq_stream_create("tasks", 32, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
        cmq_stream_consumer_t s = cmq_stream_consumer_state(st, "worker1");
        ASSERT_EQ(s.consumer_seq, (uint64_t)2);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"a", 1), (uint64_t)1);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"b", 1), (uint64_t)2);
        ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"c", 1), (uint64_t)3);
        ASSERT_EQ(cmq_stream_consumer_next(st, "worker1"), (uint64_t)3);
        cmq_stream_destroy(st);
    }
    wipe();
}

TEST(cursors, isolated) {
    mkdir(CDIR, 0755);
    wipe();
    cmq_stream_t *st = cmq_stream_create("iso", 32, 0);
    ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"x", 1), (uint64_t)1);
    ASSERT_EQ(cmq_stream_append(st, (const uint8_t *)"y", 1), (uint64_t)2);
    ASSERT_EQ(cmq_stream_add_consumer(st, "a"), 0);
    ASSERT_EQ(cmq_stream_add_consumer(st, "b"), 0);
    ASSERT_EQ(cmq_stream_consumer_ack(st, "a", 1), 0);
    cmq_stream_destroy(st);
    st = cmq_stream_create("iso", 32, 0);
    ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
    ASSERT_EQ(cmq_stream_consumer_state(st, "a").consumer_seq, (uint64_t)1);
    ASSERT_EQ(cmq_stream_consumer_state(st, "b").consumer_seq, (uint64_t)0);
    cmq_stream_destroy(st);
    wipe();
}

TEST(cursors, remove_persists) {
    mkdir(CDIR, 0755);
    wipe();
    cmq_stream_t *st = cmq_stream_create("gone", 32, 0);
    ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
    ASSERT_EQ(cmq_stream_add_consumer(st, "z"), 0);
    ASSERT_EQ(cmq_stream_remove_consumer(st, "z"), 0);
    cmq_stream_destroy(st);
    st = cmq_stream_create("gone", 32, 0);
    ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
    ASSERT_EQ(cmq_stream_consumer_next(st, "z"), (uint64_t)0);
    cmq_stream_consumer_t s = cmq_stream_consumer_state(st, "z");
    ASSERT_EQ(s.consumer_seq, (uint64_t)0);
    ASSERT_EQ(s.pending_count, (uint32_t)0);
    cmq_stream_destroy(st);
    wipe();
}

TEST(cursors, bad_dir) {
    cmq_stream_t *st = cmq_stream_create("tasks", 8, 0);
    ASSERT_NOT_NULL(st);
    ASSERT(cmq_stream_set_cursor_path(st, "/tmp/../etc") != 0);
    ASSERT(cmq_stream_set_cursor_path(st, NULL) != 0);
    cmq_stream_destroy(st);
}

TEST(cursors, reject_unsafe_name) {
    mkdir(CDIR, 0755);
    wipe();
    cmq_stream_t *st = cmq_stream_create("tasks", 8, 0);
    ASSERT_EQ(cmq_stream_set_cursor_path(st, CDIR), 0);
    ASSERT(cmq_stream_add_consumer(st, "bad name") != 0);
    ASSERT(cmq_stream_add_consumer(st, "a/b") != 0);
    ASSERT_EQ(cmq_stream_add_consumer(st, "ok_1") , 0);
    cmq_stream_destroy(st);
    wipe();
}

TEST_MAIN()
