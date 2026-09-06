/* v0.5.53: Kafka-style key compaction on sealed .1 segments. */
#include "cmq_test.h"
#include "cmq_filestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CDIR "/tmp/cmq_fs_keycompact"
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
    snprintf(p, sizeof(p), CDIR "/%s.data.1.tmp", prefix);
    remove(p);
    snprintf(p, sizeof(p), CDIR "/%s.idx.1.tmp", prefix);
    remove(p);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int archive_count(const char *idx) {
    struct stat st;
    if (stat(idx, &st) != 0) return -1;
    return (int)(st.st_size / 8);
}

static int archive_read(const char *data_path, const char *idx_path,
                        uint64_t seq, uint8_t **out, size_t *out_len) {
    if (!out || !out_len || seq == 0) return -1;
    *out = NULL;
    *out_len = 0;
    FILE *idx = fopen(idx_path, "rb");
    FILE *df = fopen(data_path, "rb");
    if (!idx || !df) {
        if (idx) fclose(idx);
        if (df) fclose(df);
        return -1;
    }
    if (fseeko(idx, (off_t)((seq - 1) * 8u), SEEK_SET) != 0) {
        fclose(idx);
        fclose(df);
        return -1;
    }
    uint8_t ib[8];
    if (fread(ib, 8, 1, idx) != 1) {
        fclose(idx);
        fclose(df);
        return -1;
    }
    uint64_t off = le64(ib);
    if (fseeko(df, (off_t)off, SEEK_SET) != 0) {
        fclose(idx);
        fclose(df);
        return -1;
    }
    uint8_t hdr[FS_HDR];
    if (fread(hdr, FS_HDR, 1, df) != 1) {
        fclose(idx);
        fclose(df);
        return -1;
    }
    uint32_t plen = le32(hdr + 14);
    uint8_t *buf = malloc(plen ? plen : 1);
    if (!buf || (plen && fread(buf, 1, plen, df) != plen)) {
        free(buf);
        fclose(idx);
        fclose(df);
        return -1;
    }
    fclose(idx);
    fclose(df);
    *out = buf;
    *out_len = plen;
    return 0;
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

TEST(keycompact, noop_without_archive) {
    mkdir(CDIR, 0755);
    wipe_prefix("none");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "none");
    ASSERT_NOT_NULL(fs);
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"x", 1, NULL), 0);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)1);
    struct stat st;
    ASSERT(stat(CDIR "/none.data.1", &st) != 0);
    cmq_filestore_destroy(fs);
    wipe_prefix("none");
}

TEST(keycompact, last_wins) {
    mkdir(CDIR, 0755);
    wipe_prefix("lw");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "lw");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "k", "v1", &na), 0);
    ASSERT_EQ(pack_kv(b, sizeof(b), "k", "v2", &nb), 0);
    uint8_t *bufs[2] = {a, b};
    size_t lens[2] = {na, nb};
    force_rotate(fs, bufs, lens, 2);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)0);
    ASSERT_EQ(archive_count(CDIR "/lw.idx.1"), 2);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/lw.idx.1"), 1);
    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT_EQ(archive_read(CDIR "/lw.data.1", CDIR "/lw.idx.1", 1, &data, &len),
              0);
    const uint8_t *key = NULL, *val = NULL;
    size_t klen = 0, vlen = 0;
    ASSERT_EQ(cmq_filestore_key_decode(data, len, &key, &klen, &val, &vlen), 0);
    ASSERT_EQ(klen, (size_t)1);
    ASSERT(memcmp(key, "k", 1) == 0);
    ASSERT_EQ(vlen, (size_t)2);
    ASSERT(memcmp(val, "v2", 2) == 0);
    free(data);
    cmq_filestore_destroy(fs);
    wipe_prefix("lw");
}

TEST(keycompact, keeps_unkeyed) {
    mkdir(CDIR, 0755);
    wipe_prefix("uk");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "uk");
    ASSERT_NOT_NULL(fs);
    uint8_t k[64];
    size_t nk = 0;
    ASSERT_EQ(pack_kv(k, sizeof(k), "a", "1", &nk), 0);
    uint8_t *bufs[3] = {(uint8_t *)"p1", k, (uint8_t *)"p2"};
    size_t lens[3] = {2, nk, 2};
    force_rotate(fs, bufs, lens, 3);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/uk.idx.1"), 3);
    cmq_filestore_destroy(fs);
    wipe_prefix("uk");
}

TEST(keycompact, tombstone_drops) {
    mkdir(CDIR, 0755);
    wipe_prefix("ts");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "ts");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], t[64];
    size_t na = 0, nt = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "gone", "old", &na), 0);
    ASSERT_EQ(cmq_filestore_key_encode(t, sizeof(t), "gone", 4, NULL, 0, &nt),
              0);
    uint8_t *bufs[2] = {a, t};
    size_t lens[2] = {na, nt};
    force_rotate(fs, bufs, lens, 2);
    ASSERT_EQ(archive_count(CDIR "/ts.idx.1"), 2);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/ts.idx.1"), 0);
    cmq_filestore_destroy(fs);
    wipe_prefix("ts");
}

TEST(keycompact, live_untouched) {
    mkdir(CDIR, 0755);
    wipe_prefix("lv");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "lv");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "k", "1", &na), 0);
    ASSERT_EQ(pack_kv(b, sizeof(b), "k", "2", &nb), 0);
    uint8_t *bufs[2] = {a, b};
    size_t lens[2] = {na, nb};
    force_rotate(fs, bufs, lens, 2);
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"live", 4, &seq), 0);
    ASSERT_EQ(seq, (uint64_t)1);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(cmq_filestore_last_seq(fs), (uint64_t)1);
    uint8_t *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cmq_filestore_read(fs, 1, &data, &len), 0);
    ASSERT_EQ(len, (size_t)4);
    ASSERT(memcmp(data, "live", 4) == 0);
    free(data);
    cmq_filestore_destroy(fs);
    wipe_prefix("lv");
}

TEST(keycompact, isolated) {
    mkdir(CDIR, 0755);
    wipe_prefix("iso");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "iso");
    ASSERT_NOT_NULL(fs);
    uint8_t a1[64], b1[64], a2[64];
    size_t n1 = 0, n2 = 0, n3 = 0;
    ASSERT_EQ(pack_kv(a1, sizeof(a1), "A", "1", &n1), 0);
    ASSERT_EQ(pack_kv(b1, sizeof(b1), "B", "1", &n2), 0);
    ASSERT_EQ(pack_kv(a2, sizeof(a2), "A", "2", &n3), 0);
    uint8_t *bufs[3] = {a1, b1, a2};
    size_t lens[3] = {n1, n2, n3};
    force_rotate(fs, bufs, lens, 3);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/iso.idx.1"), 2);
    uint8_t *d1 = NULL, *d2 = NULL;
    size_t l1 = 0, l2 = 0;
    ASSERT_EQ(archive_read(CDIR "/iso.data.1", CDIR "/iso.idx.1", 1, &d1, &l1),
              0);
    ASSERT_EQ(archive_read(CDIR "/iso.data.1", CDIR "/iso.idx.1", 2, &d2, &l2),
              0);
    const uint8_t *k = NULL, *v = NULL;
    size_t kl = 0, vl = 0;
    ASSERT_EQ(cmq_filestore_key_decode(d1, l1, &k, &kl, &v, &vl), 0);
    ASSERT(memcmp(k, "B", 1) == 0);
    ASSERT(memcmp(v, "1", 1) == 0);
    ASSERT_EQ(cmq_filestore_key_decode(d2, l2, &k, &kl, &v, &vl), 0);
    ASSERT(memcmp(k, "A", 1) == 0);
    ASSERT(memcmp(v, "2", 1) == 0);
    free(d1);
    free(d2);
    cmq_filestore_destroy(fs);
    wipe_prefix("iso");
}

TEST(keycompact, idempotent) {
    mkdir(CDIR, 0755);
    wipe_prefix("id");
    cmq_filestore_t *fs = cmq_filestore_create(CDIR, "id");
    ASSERT_NOT_NULL(fs);
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    ASSERT_EQ(pack_kv(a, sizeof(a), "k", "1", &na), 0);
    ASSERT_EQ(pack_kv(b, sizeof(b), "k", "2", &nb), 0);
    uint8_t *bufs[2] = {a, b};
    size_t lens[2] = {na, nb};
    force_rotate(fs, bufs, lens, 2);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(cmq_filestore_compact_keys(fs), 0);
    ASSERT_EQ(archive_count(CDIR "/id.idx.1"), 1);
    cmq_filestore_destroy(fs);
    wipe_prefix("id");
}

TEST_MAIN()
