/* v0.5.71: D2 HPACK Huffman (RFC 7541 C.4 / www.example.com). */
#include "cmq_test.h"
#include "cmq_hpack.h"
#include <string.h>

static const uint8_t rfc_www[] = {
    0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff
};

TEST(huff, rfc_www) {
    uint8_t enc[32];
    int n = cmq_hpack_huff_encode((const uint8_t *)"www.example.com", 15,
                                  enc, sizeof(enc));
    ASSERT_EQ(n, (int)sizeof(rfc_www));
    ASSERT(memcmp(enc, rfc_www, sizeof(rfc_www)) == 0);
    char dec[32];
    int d = cmq_hpack_huff_decode(rfc_www, sizeof(rfc_www), dec, sizeof(dec));
    ASSERT_EQ(d, 15);
    ASSERT(memcmp(dec, "www.example.com", 15) == 0);
}

TEST(huff, roundtrip) {
    const char *s = "custom-key";
    uint8_t enc[64];
    int n = cmq_hpack_huff_encode((const uint8_t *)s, 10, enc, sizeof(enc));
    ASSERT(n > 0);
    char dec[32];
    ASSERT_EQ(cmq_hpack_huff_decode(enc, (size_t)n, dec, sizeof(dec)), 10);
    ASSERT(memcmp(dec, s, 10) == 0);
}

TEST(huff, str_decode_h) {
    uint8_t wire[64];
    int hn = cmq_hpack_huff_encode((const uint8_t *)"GET", 3, wire + 1,
                                   sizeof(wire) - 1);
    ASSERT(hn > 0);
    int ln = cmq_hpack_int_encode((uint32_t)hn, 7, 0x80, wire, sizeof(wire));
    ASSERT_EQ(ln, 1);
    char out[16];
    size_t used = 0;
    ASSERT_EQ(cmq_hpack_str_decode(wire, (size_t)(1 + hn), out, sizeof(out),
                                   &used),
              0);
    ASSERT_STR_EQ(out, "GET");
    ASSERT_EQ(used, (size_t)(1 + hn));
}

TEST(huff, reject) {
    char out[8];
    char tiny[1];
    ASSERT(cmq_hpack_huff_decode(rfc_www, sizeof(rfc_www), tiny, 1) < 0);
    ASSERT(cmq_hpack_huff_encode(NULL, 1, (uint8_t *)out, 8) < 0);
    ASSERT(cmq_hpack_huff_decode(NULL, 0, out, sizeof(out)) < 0);
    uint8_t badpad[] = {0x00};
    ASSERT(cmq_hpack_huff_decode(badpad, 1, out, sizeof(out)) < 0);
}

TEST_MAIN()
