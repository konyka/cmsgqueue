/* v0.5.58: last-value KV (D4 phase 2). */
#include "cmq_test.h"
#include "cmq_kv.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CDIR "/tmp/cmq_kv"
#define PREFIX "bucket"

static void wipe(void) {
    remove(CDIR "/" PREFIX ".data");
    remove(CDIR "/" PREFIX ".idx");
    remove(CDIR "/" PREFIX ".data.1");
    remove(CDIR "/" PREFIX ".idx.1");
}

TEST(kv, put_get_overwrite) {
    cmq_kv_t *kv = cmq_kv_create(8);
    ASSERT_NOT_NULL(kv);
    ASSERT_EQ(cmq_kv_put(kv, "a", (const uint8_t *)"one", 3), 0);
    ASSERT_EQ(cmq_kv_count(kv), (size_t)1);
    uint8_t buf[16];
    size_t n = 0;
    ASSERT_EQ(cmq_kv_get(kv, "a", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(n, (size_t)3);
    ASSERT(memcmp(buf, "one", 3) == 0);
    ASSERT_EQ(cmq_kv_put(kv, "a", (const uint8_t *)"two", 3), 0);
    ASSERT_EQ(cmq_kv_count(kv), (size_t)1);
    ASSERT_EQ(cmq_kv_get(kv, "a", buf, sizeof(buf), &n), 0);
    ASSERT(memcmp(buf, "two", 3) == 0);
    cmq_kv_destroy(kv);
}

TEST(kv, del_and_isolated) {
    cmq_kv_t *kv = cmq_kv_create(8);
    ASSERT_EQ(cmq_kv_put(kv, "x", (const uint8_t *)"1", 1), 0);
    ASSERT_EQ(cmq_kv_put(kv, "y", (const uint8_t *)"2", 1), 0);
    ASSERT_EQ(cmq_kv_del(kv, "x"), 0);
    ASSERT_EQ(cmq_kv_count(kv), (size_t)1);
    uint8_t buf[8];
    size_t n = 0;
    ASSERT(cmq_kv_get(kv, "x", buf, sizeof(buf), &n) != 0);
    ASSERT_EQ(cmq_kv_get(kv, "y", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(buf[0], (uint8_t)'2');
    ASSERT(cmq_kv_del(kv, "x") != 0);
    cmq_kv_destroy(kv);
}

TEST(kv, no_persist_no_file) {
    mkdir(CDIR, 0755);
    wipe();
    cmq_kv_t *kv = cmq_kv_create(8);
    ASSERT_EQ(cmq_kv_put(kv, "a", (const uint8_t *)"z", 1), 0);
    cmq_kv_destroy(kv);
    struct stat s;
    ASSERT(stat(CDIR "/" PREFIX ".data", &s) != 0);
}

TEST(kv, reopen_restores) {
    mkdir(CDIR, 0755);
    wipe();
    {
        cmq_kv_t *kv = cmq_kv_create(8);
        ASSERT_EQ(cmq_kv_set_persist(kv, CDIR, PREFIX), 0);
        ASSERT_EQ(cmq_kv_put(kv, "k", (const uint8_t *)"v1", 2), 0);
        ASSERT_EQ(cmq_kv_put(kv, "k", (const uint8_t *)"v2", 2), 0);
        cmq_kv_destroy(kv);
    }
    {
        cmq_kv_t *kv = cmq_kv_create(8);
        ASSERT_EQ(cmq_kv_set_persist(kv, CDIR, PREFIX), 0);
        uint8_t buf[8];
        size_t n = 0;
        ASSERT_EQ(cmq_kv_get(kv, "k", buf, sizeof(buf), &n), 0);
        ASSERT_EQ(n, (size_t)2);
        ASSERT(memcmp(buf, "v2", 2) == 0);
        cmq_kv_destroy(kv);
    }
    wipe();
}

TEST(kv, tombstone_not_restored) {
    mkdir(CDIR, 0755);
    wipe();
    {
        cmq_kv_t *kv = cmq_kv_create(8);
        ASSERT_EQ(cmq_kv_set_persist(kv, CDIR, PREFIX), 0);
        ASSERT_EQ(cmq_kv_put(kv, "gone", (const uint8_t *)"x", 1), 0);
        ASSERT_EQ(cmq_kv_del(kv, "gone"), 0);
        cmq_kv_destroy(kv);
    }
    {
        cmq_kv_t *kv = cmq_kv_create(8);
        ASSERT_EQ(cmq_kv_set_persist(kv, CDIR, PREFIX), 0);
        uint8_t buf[8];
        size_t n = 0;
        ASSERT(cmq_kv_get(kv, "gone", buf, sizeof(buf), &n) != 0);
        ASSERT_EQ(cmq_kv_count(kv), (size_t)0);
        cmq_kv_destroy(kv);
    }
    wipe();
}

TEST(kv, reject_unsafe_and_full) {
    cmq_kv_t *kv = cmq_kv_create(2);
    ASSERT(cmq_kv_put(kv, "bad name", (const uint8_t *)"x", 1) != 0);
    ASSERT(cmq_kv_put(kv, "a/b", (const uint8_t *)"x", 1) != 0);
    ASSERT_EQ(cmq_kv_put(kv, "ok_1", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_kv_put(kv, "ok-2", (const uint8_t *)"y", 1), 0);
    ASSERT_EQ(cmq_kv_put(kv, "ok.3", (const uint8_t *)"z", 1), -2);
    uint8_t big[CMQ_KV_VAL_MAX + 1];
    memset(big, 'a', sizeof(big));
    ASSERT_EQ(cmq_kv_put(kv, "ok_1", big, sizeof(big)), -3);
    cmq_kv_destroy(kv);
}

TEST_MAIN()
