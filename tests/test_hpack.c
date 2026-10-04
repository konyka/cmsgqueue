/* v0.5.66: D2 HPACK static codec (RFC 7541 C.1 + indexed). */
#include "cmq_test.h"
#include "cmq_hpack.h"
#include <string.h>

TEST(hpack, int_rfc) {
    uint8_t buf[8];
    int n = cmq_hpack_int_encode(10, 5, 0, buf, sizeof(buf));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(buf[0], (uint8_t)0x0a);
    uint32_t v = 0;
    size_t used = 0;
    ASSERT_EQ(cmq_hpack_int_decode(buf, (size_t)n, 5, &v, &used), 0);
    ASSERT_EQ(v, (uint32_t)10);
    ASSERT_EQ(used, (size_t)1);

    n = cmq_hpack_int_encode(1337, 5, 0, buf, sizeof(buf));
    ASSERT_EQ(n, 3);
    ASSERT_EQ(buf[0], (uint8_t)0x1f);
    ASSERT_EQ(buf[1], (uint8_t)0x9a);
    ASSERT_EQ(buf[2], (uint8_t)0x0a);
    ASSERT_EQ(cmq_hpack_int_decode(buf, (size_t)n, 5, &v, &used), 0);
    ASSERT_EQ(v, (uint32_t)1337);

    n = cmq_hpack_int_encode(42, 8, 0, buf, sizeof(buf));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(buf[0], (uint8_t)42);
}

TEST(hpack, str_literal) {
    uint8_t buf[32];
    int n = cmq_hpack_str_encode("custom-key", buf, sizeof(buf));
    ASSERT(n > 1);
    ASSERT_EQ(buf[0] & 0x80, 0);
    char out[32];
    size_t used = 0;
    ASSERT_EQ(cmq_hpack_str_decode(buf, (size_t)n, out, sizeof(out), &used), 0);
    ASSERT_STR_EQ(out, "custom-key");
    ASSERT_EQ(used, (size_t)n);
}

TEST(hpack, indexed_get) {
    const char *name = NULL, *val = NULL;
    ASSERT_EQ(cmq_hpack_static_get(2, &name, &val), 0);
    ASSERT_STR_EQ(name, ":method");
    ASSERT_STR_EQ(val, "GET");
    uint8_t buf[4];
    int n = cmq_hpack_hdr_encode_indexed(2, buf, sizeof(buf));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(buf[0], (uint8_t)0x82);
    char hn[32], hv[32];
    size_t used = 0;
    ASSERT_EQ(cmq_hpack_hdr_decode(buf, (size_t)n, hn, sizeof(hn), hv,
                                   sizeof(hv), &used),
              0);
    ASSERT_STR_EQ(hn, ":method");
    ASSERT_STR_EQ(hv, "GET");
}

TEST(hpack, reject) {
    uint8_t buf[4];
    ASSERT(cmq_hpack_int_encode(CMQ_HPACK_INT_MAX + 1, 5, 0, buf,
                                sizeof(buf)) < 0);
    ASSERT(cmq_hpack_static_get(0, NULL, NULL) != 0);
    ASSERT(cmq_hpack_static_get(62, NULL, NULL) != 0);
    uint8_t huff[2] = {0x80, 0x00};
    char out[8];
    size_t used = 0;
    ASSERT(cmq_hpack_str_decode(huff, 2, out, sizeof(out), &used) != 0);
}

TEST_MAIN()
