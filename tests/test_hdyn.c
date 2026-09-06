/* v0.5.72: D2 HPACK 4 KiB dynamic table (RFC 7541 C.2.1 / C.3). */
#include "cmq_test.h"
#include "cmq_hpack.h"
#include <string.h>

/* C.2.1 Literal Header Field with Incremental Indexing. */
static const uint8_t rfc_c21[] = {
    0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65,
    0x79, 0x0d, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x68, 0x65,
    0x61, 0x64, 0x65, 0x72
};

/* C.3.1 First request (no Huffman). */
static const uint8_t rfc_c31[] = {
    0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78,
    0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d
};

/* C.3.2 Second request. */
static const uint8_t rfc_c32[] = {
    0x82, 0x86, 0x84, 0xbe, 0x58, 0x08, 0x6e, 0x6f, 0x2d, 0x63, 0x61,
    0x63, 0x68, 0x65
};

static int dec_block(cmq_hpack_dyn_t *t, const uint8_t *in, size_t len) {
    size_t off = 0;
    int nh = 0;
    while (off < len) {
        char name[CMQ_HPACK_STR_MAX + 1];
        char value[CMQ_HPACK_STR_MAX + 1];
        size_t used = 0;
        int r = cmq_hpack_hdr_decode_dyn(t, in + off, len - off, name,
                                         sizeof(name), value, sizeof(value),
                                         &used);
        if (r < 0 || used == 0 || used > len - off) return -1;
        off += used;
        if (r == 0) nh++;
    }
    return nh;
}

TEST(hdyn, rfc_c21) {
    cmq_hpack_dyn_t t;
    cmq_hpack_dyn_init(&t);
    uint8_t enc[64];
    int n = cmq_hpack_hdr_encode_inc(&t, "custom-key", "custom-header", enc,
                                                  sizeof(enc));
    ASSERT_EQ(n, (int)sizeof(rfc_c21));
    ASSERT(memcmp(enc, rfc_c21, sizeof(rfc_c21)) == 0);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 1u);
    ASSERT_EQ(cmq_hpack_dyn_size(&t), 55u);

    cmq_hpack_dyn_t d;
    cmq_hpack_dyn_init(&d);
    char name[32], value[32];
    size_t used = 0;
    ASSERT_EQ(cmq_hpack_hdr_decode_dyn(&d, rfc_c21, sizeof(rfc_c21), name,
                                       sizeof(name), value, sizeof(value),
                                       &used),
              0);
    ASSERT_STR_EQ(name, "custom-key");
    ASSERT_STR_EQ(value, "custom-header");
    ASSERT_EQ(used, sizeof(rfc_c21));
    ASSERT_EQ(cmq_hpack_dyn_count(&d), 1u);
    ASSERT_EQ(cmq_hpack_dyn_size(&d), 55u);
    const char *dn = NULL, *dv = NULL;
    size_t nl = 0, vl = 0;
    ASSERT_EQ(cmq_hpack_dyn_get(&d, 0, &dn, &nl, &dv, &vl), 0);
    ASSERT_EQ(nl, 10u);
    ASSERT(memcmp(dn, "custom-key", 10) == 0);
    ASSERT_EQ(vl, 13u);
    ASSERT(memcmp(dv, "custom-header", 13) == 0);
}

TEST(hdyn, rfc_c3) {
    cmq_hpack_dyn_t t;
    cmq_hpack_dyn_init(&t);
    ASSERT_EQ(dec_block(&t, rfc_c31, sizeof(rfc_c31)), 4);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 1u);
    ASSERT_EQ(cmq_hpack_dyn_size(&t), 57u);
    const char *n = NULL, *v = NULL;
    size_t nl = 0, vl = 0;
    ASSERT_EQ(cmq_hpack_dyn_get(&t, 0, &n, &nl, &v, &vl), 0);
    ASSERT(memcmp(n, ":authority", 10) == 0);
    ASSERT(memcmp(v, "www.example.com", 15) == 0);

    ASSERT_EQ(dec_block(&t, rfc_c32, sizeof(rfc_c32)), 5);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 2u);
    ASSERT_EQ(cmq_hpack_dyn_size(&t), 110u);
    ASSERT_EQ(cmq_hpack_dyn_get(&t, 0, &n, &nl, &v, &vl), 0);
    ASSERT(memcmp(n, "cache-control", 13) == 0);
    ASSERT(memcmp(v, "no-cache", 8) == 0);
    ASSERT_EQ(cmq_hpack_dyn_get(&t, 1, &n, &nl, &v, &vl), 0);
    ASSERT(memcmp(n, ":authority", 10) == 0);
}

TEST(hdyn, evict) {
    cmq_hpack_dyn_t t;
    cmq_hpack_dyn_init(&t);
    ASSERT_EQ(cmq_hpack_dyn_set_max(&t, 64), 0);
    ASSERT_EQ(cmq_hpack_dyn_add(&t, "custom-key", "custom-header"), 0);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 1u);
    ASSERT_EQ(cmq_hpack_dyn_add(&t, "custom-key", "custom-header"), 0);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 1u);
    ASSERT_EQ(cmq_hpack_dyn_size(&t), 55u);
    ASSERT_EQ(cmq_hpack_dyn_set_max(&t, 30), 0);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 0u);
    ASSERT_EQ(cmq_hpack_dyn_add(&t, "custom-key", "custom-header"), 0);
    ASSERT_EQ(cmq_hpack_dyn_count(&t), 0u);
    ASSERT_EQ(cmq_hpack_dyn_size(&t), 0u);
}

TEST(hdyn, reject) {
    cmq_hpack_dyn_t t;
    cmq_hpack_dyn_init(&t);
    char name[8], value[8];
    size_t used = 0;
    uint8_t zidx[] = {0x80};
    ASSERT(cmq_hpack_hdr_decode_dyn(&t, zidx, 1, name, sizeof(name), value,
                                    sizeof(value), &used) < 0);
    uint8_t miss[] = {0xbe};
    ASSERT(cmq_hpack_hdr_decode_dyn(&t, miss, 1, name, sizeof(name), value,
                                    sizeof(value), &used) < 0);
    ASSERT(cmq_hpack_dyn_set_max(&t, CMQ_HPACK_DYN_MAX + 1) < 0);
    uint8_t bigsz[8];
    int bn = cmq_hpack_int_encode(CMQ_HPACK_DYN_MAX + 1, 5, 0x20, bigsz,
                                  sizeof(bigsz));
    ASSERT(bn > 0);
    ASSERT(cmq_hpack_hdr_decode_dyn(&t, bigsz, (size_t)bn, name, sizeof(name),
                                    value, sizeof(value), &used) < 0);
    ASSERT(cmq_hpack_dyn_get(&t, 0, NULL, NULL, NULL, NULL) < 0);
    ASSERT(cmq_hpack_hdr_decode_dyn(NULL, rfc_c21, 1, name, 8, value, 8,
                                    &used) < 0);
}

TEST_MAIN()
