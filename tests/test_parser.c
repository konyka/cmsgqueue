#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include <string.h>
#include <stdlib.h>

TEST(parser, create_destroy) {
    cmq_parser_t *p = cmq_parser_create();
    ASSERT_NOT_NULL(p);
    cmq_parser_destroy(p);
}

TEST(parser, feed_header_only_ping) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = 0;
    hdr.op = CMQ_OP_PING;
    hdr.length = 0;

    int rc = cmq_parser_feed(p, (const uint8_t *)&hdr, sizeof(hdr));
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *frame = cmq_parser_frame(p);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->hdr.op, CMQ_OP_PING);
    ASSERT_EQ(frame->hdr.length, (cmq_u32_t)0);
    ASSERT_EQ(frame->payload_len, (size_t)0);

    cmq_parser_destroy(p);
}

TEST(parser, feed_header_and_payload) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[64];
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = 0;
    hdr.op = CMQ_OP_PUBLISH;
    hdr.length = 5;
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), "hello", 5);

    int rc = cmq_parser_feed(p, buf, sizeof(hdr) + 5);
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *frame = cmq_parser_frame(p);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->hdr.op, CMQ_OP_PUBLISH);
    ASSERT_EQ(frame->payload_len, (size_t)5);
    ASSERT(memcmp(frame->payload, "hello", 5) == 0);

    cmq_parser_destroy(p);
}

TEST(parser, feed_byte_by_byte) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[64];
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = 0;
    hdr.op = CMQ_OP_SUBSCRIBE;
    hdr.length = 3;
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), "abc", 3);

    int total = sizeof(hdr) + 3;
    for (int i = 0; i < total - 1; i++) {
        int rc = cmq_parser_feed(p, &buf[i], 1);
        ASSERT_EQ(rc, 0);
    }
    int rc = cmq_parser_feed(p, &buf[total - 1], 1);
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *frame = cmq_parser_frame(p);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(frame->hdr.op, CMQ_OP_SUBSCRIBE);
    ASSERT_EQ(frame->payload_len, (size_t)3);

    cmq_parser_destroy(p);
}

TEST(parser, multiple_frames) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[128];
    size_t off = 0;

    cmq_frame_hdr_t h1;
    h1.magic[0] = CMQ_PROTO_MAGIC_0;
    h1.magic[1] = CMQ_PROTO_MAGIC_1;
    h1.version = CMQ_PROTO_VERSION;
    h1.flags = 0;
    h1.op = CMQ_OP_PING;
    h1.length = 0;
    memcpy(buf + off, &h1, sizeof(h1));
    off += sizeof(h1);

    cmq_frame_hdr_t h2;
    h2.magic[0] = CMQ_PROTO_MAGIC_0;
    h2.magic[1] = CMQ_PROTO_MAGIC_1;
    h2.version = CMQ_PROTO_VERSION;
    h2.flags = 0;
    h2.op = CMQ_OP_PONG;
    h2.length = 0;
    memcpy(buf + off, &h2, sizeof(h2));
    off += sizeof(h2);

    int rc = cmq_parser_feed(p, buf, off);
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *f1 = cmq_parser_frame(p);
    ASSERT_EQ(f1->hdr.op, CMQ_OP_PING);

    rc = cmq_parser_next(p);
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *f2 = cmq_parser_frame(p);
    ASSERT_EQ(f2->hdr.op, CMQ_OP_PONG);

    rc = cmq_parser_next(p);
    ASSERT_EQ(rc, 0);

    cmq_parser_destroy(p);
}

TEST(parser, invalid_magic) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[8] = {0xDE, 0xAD, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};

    int rc = cmq_parser_feed(p, buf, 8);
    ASSERT(rc < 0);

    cmq_parser_destroy(p);
}

TEST(parser, wrong_version) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[8] = {CMQ_PROTO_MAGIC_0, CMQ_PROTO_MAGIC_1, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00};

    int rc = cmq_parser_feed(p, buf, 8);
    ASSERT(rc < 0);

    cmq_parser_destroy(p);
}

TEST(parser, reset) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_reset(p);
    cmq_parser_destroy(p);
}

TEST(parser, frame_encode) {
    uint8_t buf[64];
    const char *payload = "test payload";
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH, 0, (const uint8_t *)payload, strlen(payload));
    ASSERT(len > 0);

    cmq_frame_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    ASSERT_EQ(hdr.magic[0], CMQ_PROTO_MAGIC_0);
    ASSERT_EQ(hdr.magic[1], CMQ_PROTO_MAGIC_1);
    ASSERT_EQ(hdr.version, CMQ_PROTO_VERSION);
    ASSERT_EQ(hdr.op, CMQ_OP_PUBLISH);
    ASSERT_EQ(hdr.length, (cmq_u32_t)strlen(payload));
    ASSERT(memcmp(buf + sizeof(hdr), payload, strlen(payload)) == 0);
}

TEST(parser, frame_encode_buffer_too_small) {
    uint8_t buf[4];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PING, 0, NULL, 0);
    ASSERT_EQ(len, (size_t)0);
}

TEST(parser, large_payload) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t bigpayload[4096];
    memset(bigpayload, 'X', sizeof(bigpayload));

    uint8_t buf[4200];
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = 0;
    hdr.op = CMQ_OP_PUBLISH;
    hdr.length = sizeof(bigpayload);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), bigpayload, sizeof(bigpayload));

    int rc = cmq_parser_feed(p, buf, sizeof(hdr) + sizeof(bigpayload));
    ASSERT_EQ(rc, 1);

    const cmq_frame_t *frame = cmq_parser_frame(p);
    ASSERT_EQ(frame->payload_len, sizeof(bigpayload));

    cmq_parser_destroy(p);
}

TEST_MAIN()
