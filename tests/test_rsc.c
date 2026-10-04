/* v0.5.172: strip+verify CMQ_FLAG_CHECKSUM before RESPONSE parse. */
#include "cmq_crc32c.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include <string.h>

/* RESPONSE-shaped payload: u16 subject + subject + u16 reply + body. */
static size_t build_response(uint8_t *buf) {
    size_t n = 0;
    buf[n++] = 0;
    buf[n++] = 2;
    buf[n++] = 'o';
    buf[n++] = 'k';
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 'h';
    buf[n++] = 'i';
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

TEST(rsc, apply) {
    uint8_t buf[32];
    size_t n = append_crc(buf, build_response(buf));
    ASSERT_EQ(n, 12u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n), 0);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[2], 'o');
    ASSERT_EQ(buf[3], 'k');
}

TEST(rsc, omitted) {
    uint8_t buf[32];
    size_t n = build_response(buf);
    ASSERT_EQ(cmq_checksum_consume(0, buf, &n), 0);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(cmq_checksum_consume(CMQ_FLAG_HEADERS, buf, &n), 0);
    ASSERT_EQ(n, 8u);
}

TEST(rsc, empty) {
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

TEST(rsc, reject) {
    uint8_t buf[32];
    size_t n = append_crc(buf, build_response(buf));
    buf[2] ^= 0x01;
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, &n) != 0);
    ASSERT_EQ(n, 12u);
    ASSERT(cmq_checksum_consume(CMQ_FLAG_CHECKSUM, buf, NULL) != 0);
}

TEST_MAIN()
