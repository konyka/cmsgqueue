/* v0.5.171: strip+verify CMQ_FLAG_CHECKSUM before REQUEST parse. */
#include "cmq_crc32c.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include <string.h>

static size_t append_crc(uint8_t *buf, size_t n) {
    uint32_t crc = cmq_crc32c(0, buf, n);
    buf[n++] = (uint8_t)(crc & 0xFF);
    buf[n++] = (uint8_t)((crc >> 8) & 0xFF);
    buf[n++] = (uint8_t)((crc >> 16) & 0xFF);
    buf[n++] = (uint8_t)((crc >> 24) & 0xFF);
    return n;
}

TEST(csa, apply) {
    uint8_t buf[16];
    memcpy(buf, "hi", 2);
    size_t n = append_crc(buf, 2);
    ASSERT_EQ(n, 6u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n), 0);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(buf[0], 'h');
    ASSERT_EQ(buf[1], 'i');
}

TEST(csa, omitted) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    size_t n = 8;
    ASSERT_EQ(cmq_checksum_consume(0, buf, &n), 0);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_HEADERS, buf, &n), 0);
    ASSERT_EQ(n, 8u);
}

TEST(csa, empty) {
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

TEST(csa, reject) {
    uint8_t buf[8];
    memcpy(buf, "hi", 2);
    size_t n = append_crc(buf, 2);
    buf[0] ^= 0x01;
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n) != 0);
    ASSERT_EQ(n, 6u);
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, NULL) != 0);
}

TEST_MAIN()
