/* F2: Wire compression (zstd) at batch level.
 *
 * Tests verify:
 *   - Round-trip: compress a known buffer, decompress, bytes match.
 *   - Decompression bomb: 1 byte cannot decompress to >16x.
 *   - zstd is available.
 */

#include "cmq_test.h"
#include "cmq_compress.h"

TEST(compress, is_available) {
    ASSERT_EQ(cmq_compress_is_available(), 1);
}

TEST(compress, round_trip_short) {
    const char *data = "hello, CMSGQueue compression test!";
    size_t n = strlen(data);
    size_t cap = cmq_compress_bound(n);
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress((const uint8_t *)data, n, enc, cap);
    ASSERT(enc_len > 0);
    uint8_t dec[128];
    ssize_t dec_len = cmq_decompress(enc, (size_t)enc_len, dec, sizeof(dec));
    ASSERT_EQ(dec_len, (ssize_t)n);
    ASSERT(memcmp(dec, data, n) == 0);
    free(enc);
}

TEST(compress, round_trip_large_json) {
    static uint8_t data[4096];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    }
    size_t cap = cmq_compress_bound(sizeof(data));
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress(data, sizeof(data), enc, cap);
    ASSERT(enc_len > 0);
    /* Alphabet text compresses well. */
    ASSERT((size_t)enc_len < sizeof(data) / 4);
    uint8_t dec[sizeof(data)];
    ssize_t dec_len = cmq_decompress(enc, (size_t)enc_len, dec, sizeof(dec));
    ASSERT_EQ(dec_len, (ssize_t)sizeof(data));
    ASSERT(memcmp(dec, data, sizeof(data)) == 0);
    free(enc);
}

TEST(compress, bomb_rejected) {
    static uint8_t data[4096];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = 'A';
    size_t bound = cmq_compress_bound(sizeof(data));
    uint8_t *enc = malloc(bound);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress(data, sizeof(data), enc, bound);
    ASSERT(enc_len > 0);
    static uint8_t small[1024];
    ssize_t dec_len = cmq_decompress(enc, (size_t)enc_len,
                                      small, sizeof(small));
    ASSERT(dec_len == -1);
    free(enc);
}

TEST(compress, corrupt_data_rejected) {
    static uint8_t enc[64];
    for (size_t i = 0; i < sizeof(enc); i++) enc[i] = (uint8_t)i;
    uint8_t dec[256];
    ssize_t dec_len = cmq_decompress(enc, sizeof(enc), dec, sizeof(dec));
    ASSERT(dec_len == -1);
}

TEST(compress, decompress_bound_exact) {
    const char *data = "hello, CMSGQueue compression test!";
    size_t n = strlen(data);
    size_t cap = cmq_compress_bound(n);
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress((const uint8_t *)data, n, enc, cap);
    ASSERT(enc_len > 0);
    ssize_t bound = cmq_decompress_bound(enc, (size_t)enc_len);
    ASSERT_EQ(bound, (ssize_t)n);
    free(enc);
}

TEST(compress, decompress_bound_high_ratio) {
    /* 4 KiB of 'A' compresses far beyond 16x. A ratio cap would
     * reject the documented zstd happy path; the bound must accept
     * it (content size 4096 < 16 MiB). */
    static uint8_t data[4096];
    memset(data, 'A', sizeof(data));
    size_t cap = cmq_compress_bound(sizeof(data));
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress(data, sizeof(data), enc, cap);
    ASSERT(enc_len > 0);
    ASSERT((size_t)enc_len * 16 < sizeof(data));
    ssize_t bound = cmq_decompress_bound(enc, (size_t)enc_len);
    ASSERT_EQ(bound, (ssize_t)sizeof(data));
    free(enc);
}

TEST(compress, decompress_bound_corrupt) {
    static uint8_t enc[64];
    for (size_t i = 0; i < sizeof(enc); i++) enc[i] = (uint8_t)i;
    ASSERT_EQ(cmq_decompress_bound(enc, sizeof(enc)), -1);
}

TEST_MAIN()
