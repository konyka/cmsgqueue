/* v0.5.88: tombstone TTL + dirty-ratio key compact. */
#include "cmq_test.h"
#include "cmq_filestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CDIR "/tmp/cmq_fs_kttl"
#define FS_HDR 22

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
}

static int archive_count(const char *idx) {
    struct stat st;
    if (stat(idx, &st) != 0) return -1;
    return (int)(st.st_size / 8);
}

static int pack_kv(uint8_t *out, size_t cap, const char *k, const char *v,
                   size_t *n) {
    return cmq_filestore_key_encode(out, cap, k, strlen(k),
                                    (const uint8_t *)v, strlen(v), n);
}

static void force_rotate(cmq_filestore_t *fs, uint8_t **bufs, size_t *lens,
                         int n) {
    size_t bytes = 0;
    for (int i = 0; i < n; i++)
        bytes += (size_t)FS_HDR + lens[i];
    cmq_filestore_set_rotate_bytes(fs, bytes);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(cmq_filestore_append(fs, bufs[i], lens[i], NULL), 0);
}

TEST(kttl, dirty) {
    mkdir(CDIR, 0755);
    wipe_prefix("d");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "d");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "k", "1", &na), 0);
    ASSERT_EQ(pack_kv(b, sizeof(b), "k", "2", &nb), 0);
    uint8_t *bufs[2] = {a, b};
    size_t lens[2] = {na, nb};
    force_rotate(fs, bufs, lens, 2);
    size_t drop = 0, total = 0;
    ASSERT_EQ(cmq_filestore_key_dirty(fs, &drop, &total), 0);
    ASSERT_EQ(drop, (size_t)1);
    ASSERT_EQ(total, (size_t)2);
    ASSERT_EQ(cmq_filestore_set_compact_dirty(fs, 1, 2), 0);
    ASSERT_EQ(cmq_filestore_compact_keys_maybe(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/d.idx.1"), 1);
    cmq_filestore_destroy(fs);
    wipe_prefix("d");
}

TEST(kttl, ttl_keeps_tombstone) {
    mkdir(CDIR, 0755);
    wipe_prefix("t");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "t");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], tomb[64];
    size_t na = 0, nt = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "gone", "old", &na), 0);
    ASSERT_EQ(cmq_filestore_key_encode(tomb, sizeof(tomb), "gone", 4, NULL, 0,
                                      &nt), 0);
    uint8_t *bufs[2] = {a, tomb};
    size_t lens[2] = {na, nt};
    force_rotate(fs, bufs, lens, 2);
    cmq_filestore_set_tombstone_ttl_ms(fs, 3600000);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/t.idx.1"), 1);
    cmq_filestore_set_tombstone_ttl_ms(fs, 0);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/t.idx.1"), 0);
    cmq_filestore_destroy(fs);
    wipe_prefix("t");
}

TEST(kttl, auto_after_rotate) {
    mkdir(CDIR, 0755);
    wipe_prefix("a");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "a");
    ASSERT_NOT_NULL(fs);
    ASSERT_EQ(cmq_filestore_set_compact_dirty(fs, 1, 2), 0);
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "k", "1", &na), 0);
    ASSERT_EQ(pack_kv(b, sizeof(b), "k", "2", &nb), 0);
    uint8_t *bufs[2] = {a, b};
    size_t lens[2] = {na, nb};
    force_rotate(fs, bufs, lens, 2);
    ASSERT_EQ(archive_count(CDIR "/a.idx.1"), 1);
    cmq_filestore_destroy(fs);
    wipe_prefix("a");
}

TEST(kttl, reject) {
    ASSERT_EQ(cmq_filestore_set_compact_dirty(NULL, 1, 2), -1);
    cmq_filestore_set_tombstone_ttl_ms(NULL, 1);
    ASSERT_EQ(cmq_filestore_key_dirty(NULL, NULL, NULL), -1);
    ASSERT_EQ(cmq_filestore_compact_keys_maybe(NULL), -1);
    mkdir(CDIR, 0755);
    wipe_prefix("r");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "r");
    ASSERT_NOT_NULL(fs);
    ASSERT_EQ(cmq_filestore_set_compact_dirty(fs, 1, 0), 0);
    ASSERT_EQ(cmq_filestore_compact_keys_maybe(fs), 0);
    size_t drop = 9, total = 9;
    ASSERT_EQ(cmq_filestore_key_dirty(fs, &drop, &total), 0);
    ASSERT_EQ(drop, (size_t)0);
    ASSERT_EQ(total, (size_t)0);
    cmq_filestore_destroy(fs);
    wipe_prefix("r");
}

TEST_MAIN()
