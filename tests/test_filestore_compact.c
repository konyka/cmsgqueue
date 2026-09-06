/* v0.5.45: WAL compact + size-cap rotate. */
#include "cmq_test.h"
#include "cmq_filestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CDIR "/tmp/cmq_fs_compact"

static void wipe_prefix(const char *prefix) {
    char p[256];
    snprintf(p, sizeof(p), CDIR "/%s.data", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.idx", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.data.1", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.idx.1", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.data.tmp", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.idx.tmp", prefix);
    remove(p);
}

TEST(filestore, compact_keeps_tail) {
    mkdir(CDIR, 0755);
    wipe_prefix("tail");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "tail");
    ASSERT_NOT_NULL(fs);
    for (int i = 0; i < 5; i++) {
        char b[8];
        snprintf(b, sizeof(b), "r%d", i + 1);
        uint64_t seq = 0;
        ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)b, strlen(b), &seq), 0);
        ASSERT_EQ(seq, (uint64_t)(i + 1));
    }
    ASSERT_EQ(cmq_filestore_compact(fs, 100), 0);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)5);
    ASSERT_EQ(cmq_filestore_compact(fs, 2), 0);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)2);

    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
    ASSERT_EQ(len, (size_t)2);
    ASSERT(memcmp(data, "r4", 2) == 0);
    free(data);
    ASSERT_EQ(cmq_filestore_read(fs, 2, &data, &len), 0);
    ASSERT(memcmp(data, "r5", 2) == 0);
    free(data);
    ASSERT(cmq_filestore_read(fs, 3, &data, &len) != 0);

    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"r6", 2, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)3);
    cmq_filestore_destroy(fs);
    wipe_prefix("tail");
}

TEST(filestore, compact_zero_empties) {
    mkdir(CDIR, 0755);
    wipe_prefix("empty");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "empty");
    ASSERT_NOT_NULL(fs);
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"a", 1, NULL), 0);
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"b", 1, NULL), 0);
    ASSERT_EQ(cmq_filestore_compact(fs, 0), 0);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)0);
    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT(cmq_filestore_read(fs, 1, &data, &len) != 0);
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"c", 1, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)1);
    cmq_filestore_destroy(fs);
    wipe_prefix("empty");
}

TEST(filestore, compact_survives_reopen) {
    mkdir(CDIR, 0755);
    wipe_prefix("reopen");
    {
        cmq_filestore_t *fs = cmq_filestore_create(CDIR, "reopen");
        ASSERT_NOT_NULL(fs);
        ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"aa", 2, NULL), 0);
        ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"bb", 2, NULL), 0);
        ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"cc", 2, NULL), 0);
        ASSERT_EQ(cmq_filestore_compact(fs, 1), 0);
        cmq_filestore_sync(fs);
        cmq_filestore_destroy(fs);
    }
    {
        cmq_filestore_t *fs = cmq_filestore_create(CDIR, "reopen");
        ASSERT_NOT_NULL(fs);
        ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)1);
        uint8_t *data = NULL;
        size_t len = 0;
        ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
        ASSERT(memcmp(data, "cc", 2) == 0);
        free(data);
        cmq_filestore_destroy(fs);
    }
    wipe_prefix("reopen");
}

TEST(filestore, rotate_bytes_archives) {
    mkdir(CDIR, 0755);
    wipe_prefix("rot");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "rot");
    ASSERT_NOT_NULL(fs);
    /* 22-byte hdr + 2-byte payload = 24; two records = 48. */
    cmq_filestore_set_rotate_bytes(fs, 48);
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"aa", 2, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)1);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)1);
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"bb", 2, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)2);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)0);

    struct stat st;
    ASSERT_EQ(stat(CDIR "/rot.data.1", &st), 0);
    ASSERT(st.st_size >= 48);
    ASSERT_EQ(stat(CDIR "/rot.idx.1", &st), 0);

    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"cc", 2, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)1);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)1);
    cmq_filestore_destroy(fs);
    wipe_prefix("rot");
}

TEST_MAIN()
