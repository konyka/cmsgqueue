/* v0.5.173: strip+verify CMQ_FLAG_CHECKSUM before BATCH parse. */
#include "cmq_crc32c.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include <string.h>

/* BATCH-shaped payload: u16 count + one entry (subj/reply/body). */
static size_t build_batch(uint8_t *buf) {
    size_t n = 0;
    buf[n++] = 0;
    buf[n++] = 1;
    buf[n++] = 0;
    buf[n++] = 1;
    buf[n++] = 'a';
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 1;
    buf[n++] = 'x';
    return n;
}

static size_t append_crc(uint8_t *buf, size_t n) {
    uint32_t crc = cmq_crc32c(0, buf, n);
    buf[n++] = (uint8_t)(crc & 0xFF);
    buf[n++] = (uint8_t)((crc >> 8) & 0xFF);
    buf[n++] = (uint8_t)((crc >> 16) & 0xFF);
    buf[n++] = (uint8_t)((crc >> 24) & 0xFF);
    return n;
}

TEST(bsc, apply) {
    uint8_t buf[32];
    size_t n = append_crc(buf, build_batch(buf));
    ASSERT_EQ(n, 16u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n), 0);
    ASSERT_EQ(n, 12u);
    ASSERT_EQ(buf[4], 'a');
    ASSERT_EQ(buf[11], 'x');
}

TEST(bsc, omitted) {
    uint8_t buf[32];
    size_t n = build_batch(buf);
    ASSERT_EQ(cmq_checksum_consume(0, buf, &n), 0);
    ASSERT_EQ(n, 12u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_HEADERS, buf, &n), 0);
    ASSERT_EQ(n, 12u);
}

TEST(bsc, empty) {
    size_t n = 0;
    ASSERT_EQ(cmq_checksum_consume(0, NULL, &n), 0);
    ASSERT_EQ(n, 0u);
    n = 4;
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, NULL, &n) != 0);
    ASSERT_EQ(n, 4u);
    n = 0;
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, (const uint8_t *)"", &n) != 0);
    ASSERT_EQ(n, 0u);
}

TEST(bsc, reject) {
    uint8_t buf[32];
    size_t n = append_crc(buf, build_batch(buf));
    buf[4] ^= 0x01;
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n) != 0);
    ASSERT_EQ(n, 16u);
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, NULL) != 0);
}

TEST_MAIN()
