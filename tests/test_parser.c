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

/* More than CMQ_PARSER_FRAME_QUEUE_MAX frames in one feed: keep queued, return 1. */
TEST(parser, frame_queue_cap) {
    cmq_parser_t *p = cmq_parser_create();
    /* 65 zero-payload PING frames (header = 9 bytes typically) */
    enum { N = 65 };
    uint8_t buf[N * 16];
    size_t off = 0;
    for (int i = 0; i < N; i++) {
        cmq_frame_hdr_t h;
        h.magic[0] = CMQ_PROTO_MAGIC_0;
        h.magic[1] = CMQ_PROTO_MAGIC_1;
        h.version = CMQ_PROTO_VERSION;
        h.flags = 0;
        h.op = CMQ_OP_PING;
        h.length = 0;
        memcpy(buf + off, &h, sizeof(h));
        off += sizeof(h);
    }
    int rc = cmq_parser_feed(p, buf, off);
    /* Partial success — already-queued frames must remain drainable. */
    ASSERT_EQ(rc, 1);
    ASSERT(cmq_parser_frame(p) != NULL);
    int nq = 0;
    while (cmq_parser_frame(p)) {
        nq++;
        (void)cmq_parser_next(p);
    }
    ASSERT(nq == 64);
    /* Without new bytes, stuck complete frame must become queueable. */
    ASSERT_EQ(cmq_parser_drain_inbuf(p), 1);
    ASSERT(cmq_parser_frame(p) != NULL);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    cmq_parser_destroy(p);
}

/* Queued payload bytes capped at ~2× max_payload (not 64×). */
TEST(parser, queued_bytes_budget) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_set_max_payload(p, 64);
    uint8_t body[64];
    memset(body, 'Q', sizeof(body));
    uint8_t frame[sizeof(cmq_frame_hdr_t) + 64];
    cmq_frame_hdr_t h;
    h.magic[0] = CMQ_PROTO_MAGIC_0;
    h.magic[1] = CMQ_PROTO_MAGIC_1;
    h.version = CMQ_PROTO_VERSION;
    h.flags = 0;
    h.op = CMQ_OP_PUBLISH;
    h.length = 64;
    memcpy(frame, &h, sizeof(h));
    memcpy(frame + sizeof(h), body, 64);

    ASSERT(cmq_parser_feed(p, frame, sizeof(frame)) >= 0);
    ASSERT(cmq_parser_feed(p, frame, sizeof(frame)) >= 0);
    /* Third 64-byte frame exceeds 2×64 budget — backpressure, keep queue. */
    ASSERT_EQ(cmq_parser_feed(p, frame, sizeof(frame)), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    ASSERT(cmq_parser_frame(p) != NULL);
    ASSERT_EQ(cmq_parser_next(p), 1);
    ASSERT_EQ(cmq_parser_drain_inbuf(p), 1);
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
    size_t plen = strlen(payload);
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH, 0, (const uint8_t *)payload, plen);
    ASSERT(len > 0);
    ASSERT_EQ(buf[0], CMQ_PROTO_MAGIC_0);
    ASSERT_EQ(buf[1], CMQ_PROTO_MAGIC_1);
    ASSERT_EQ(buf[2], CMQ_PROTO_VERSION);
    ASSERT_EQ(buf[4], (uint8_t)CMQ_OP_PUBLISH);
    /* length is always little-endian on the wire */
    ASSERT_EQ(buf[5], (uint8_t)(plen & 0xff));
    ASSERT_EQ(buf[6], (uint8_t)((plen >> 8) & 0xff));
    ASSERT_EQ(buf[7], (uint8_t)((plen >> 16) & 0xff));
    ASSERT_EQ(buf[8], (uint8_t)((plen >> 24) & 0xff));
    ASSERT(memcmp(buf + CMQ_PROTO_HDR_SIZE, payload, plen) == 0);
}

TEST(parser, frame_encode_buffer_too_small) {
    uint8_t buf[4];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PING, 0, NULL, 0);
    ASSERT_EQ(len, (size_t)0);
}

TEST(parser, frame_encode_null_payload) {
    uint8_t buf[64];
    ASSERT_EQ(cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH, 0, NULL, 8),
              (size_t)0);
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

/* Declared length above cmq_parser_set_max_payload must fail before alloc. */
TEST(parser, max_payload_reject) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_set_max_payload(p, 100);

    uint8_t body[200];
    memset(body, 'Y', sizeof(body));
    uint8_t buf[256];
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = 0;
    hdr.op = CMQ_OP_PUBLISH;
    hdr.length = sizeof(body);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), body, sizeof(body));

    ASSERT_EQ(cmq_parser_feed(p, buf, sizeof(hdr) + sizeof(body)), -1);
    cmq_parser_destroy(p);
}

/* Body ceiling is 16MB; set_max_payload must accept body+overhead hard_cap. */
TEST(parser, frame_hard_cap_above_body) {
    cmq_parser_t *p = cmq_parser_create();
    size_t hard = (16u * 1024u * 1024u) + 1024u;
    cmq_parser_set_max_payload(p, hard);
    uint8_t body[32];
    memset(body, 'Z', sizeof(body));
    uint8_t buf[64];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PING, 0, body, sizeof(body));
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, buf, n), 1);
    cmq_parser_destroy(p);
}

/* Valid frame then trailing garbage: keep queued frame (partial=1). */
TEST(parser, good_then_bad_magic) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PING, 0, NULL, 0);
    ASSERT(n > 0 && n + 4 <= sizeof(buf));
    buf[n] = 0xDE;
    buf[n + 1] = 0xAD;
    buf[n + 2] = 0x01;
    buf[n + 3] = 0x00;
    ASSERT_EQ(cmq_parser_feed(p, buf, n + 4), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT(f != NULL);
    ASSERT_EQ(f->hdr.op, CMQ_OP_PING);
    cmq_parser_destroy(p);
}

/* Cross-feed: queued frame must survive a later bad prefix (drain-then-die). */
TEST(parser, good_feed_then_bad_feed) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t good[16];
    size_t n = cmq_frame_encode(good, sizeof(good), CMQ_OP_PING, 0, NULL, 0);
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, good, n), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    uint8_t bad[4] = { 0xDE, 0xAD, 0x01, 0x00 };
    ASSERT_EQ(cmq_parser_feed(p, bad, sizeof(bad)), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT(f != NULL);
    ASSERT_EQ(f->hdr.op, CMQ_OP_PING);
    ASSERT_EQ(cmq_parser_next(p), 0);
    ASSERT_EQ(cmq_parser_feed(p, bad, sizeof(bad)), -1);
    cmq_parser_destroy(p);
}

/* Near-full incomplete frame + pipelined bytes: chunked feed must complete. */
TEST(parser, inbuf_hard_cap) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_set_max_payload(p, 64);
    /* Valid header claiming 64-byte payload, but only partial body. */
    uint8_t hdr[9];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = CMQ_PROTO_MAGIC_0;
    hdr[1] = CMQ_PROTO_MAGIC_1;
    hdr[2] = CMQ_PROTO_VERSION;
    hdr[4] = CMQ_OP_PUBLISH;
    hdr[5] = 64; /* length LE = 64 */
    uint8_t body[40];
    memset(body, 0xAB, sizeof(body));
    ASSERT(cmq_parser_feed(p, hdr, sizeof(hdr)) >= 0);
    ASSERT(cmq_parser_feed(p, body, sizeof(body)) >= 0); /* 9+40; room=24 */
    /* 40 > room: absorb 24 (complete), then remainder — not all-or-nothing -1. */
    ASSERT_EQ(cmq_parser_feed(p, body, sizeof(body)), 1);
    ASSERT(cmq_parser_frame(p) != NULL);
    ASSERT_EQ(cmq_parser_frame(p)->hdr.op, CMQ_OP_PUBLISH);
    cmq_parser_destroy(p);
}

/* Completing bytes + next-frame header in one feed must not tear down. */
TEST(parser, near_full_pipeline) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_set_max_payload(p, 64);
    uint8_t hdr[9];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = CMQ_PROTO_MAGIC_0;
    hdr[1] = CMQ_PROTO_MAGIC_1;
    hdr[2] = CMQ_PROTO_VERSION;
    hdr[4] = CMQ_OP_PUBLISH;
    hdr[5] = 64;
    uint8_t partial[44];
    memset(partial, 'P', sizeof(partial));
    ASSERT(cmq_parser_feed(p, hdr, sizeof(hdr)) >= 0);
    ASSERT(cmq_parser_feed(p, partial, sizeof(partial)) >= 0); /* used=53, room=20 */
    uint8_t pipe[20 + 16];
    memset(pipe, 'P', 20);
    size_t pn = cmq_frame_encode(pipe + 20, sizeof(pipe) - 20, CMQ_OP_PING, 0,
                                  NULL, 0);
    ASSERT(pn > 0);
    ASSERT_EQ(cmq_parser_feed(p, pipe, 20 + pn), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    ASSERT(cmq_parser_frame(p) != NULL);
    ASSERT_EQ(cmq_parser_frame(p)->hdr.op, CMQ_OP_PUBLISH);
    ASSERT_EQ(cmq_parser_next(p), 1);
    ASSERT_EQ(cmq_parser_frame(p)->hdr.op, CMQ_OP_PING);
    cmq_parser_destroy(p);
}

/* Hard-cap reject must not drop already-queued frames (rc<0⇒teardown). */
TEST(parser, hard_cap_keeps_queued) {
    cmq_parser_t *p = cmq_parser_create();
    cmq_parser_set_max_payload(p, 64);
    uint8_t body[64];
    memset(body, 'H', sizeof(body));
    uint8_t frame[sizeof(cmq_frame_hdr_t) + 64];
    cmq_frame_hdr_t h;
    h.magic[0] = CMQ_PROTO_MAGIC_0;
    h.magic[1] = CMQ_PROTO_MAGIC_1;
    h.version = CMQ_PROTO_VERSION;
    h.flags = 0;
    h.op = CMQ_OP_PUBLISH;
    h.length = 64;
    memcpy(frame, &h, sizeof(h));
    memcpy(frame + sizeof(h), body, 64);
    /* Two frames fill the 2× budget; third sticks in inbuf (used == hard). */
    ASSERT(cmq_parser_feed(p, frame, sizeof(frame)) >= 0);
    ASSERT(cmq_parser_feed(p, frame, sizeof(frame)) >= 0);
    ASSERT_EQ(cmq_parser_feed(p, frame, sizeof(frame)), 1);
    uint8_t extra = 0xAB;
    ASSERT_EQ(cmq_parser_feed(p, &extra, 1), 1);
    /* Unread byte parked in hold — not fatal (queue-full backpressure). */
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    ASSERT(cmq_parser_frame(p) != NULL);
    ASSERT_EQ(cmq_parser_frame(p)->hdr.op, CMQ_OP_PUBLISH);
    ASSERT_EQ(cmq_parser_next(p), 1);
    ASSERT_EQ(cmq_parser_drain_inbuf(p), 1);
    ASSERT(cmq_parser_frame(p) != NULL);
    cmq_parser_destroy(p);
}

/* F11: Reject unknown CMQ_FLAG_* bits pre-CONNACK.
 * The flags 0x01 (CMQ_FLAG_COMPRESSED) and 0x02 (CMQ_FLAG_CHECKSUM) are
 * reserved but not yet implemented on the wire. A peer setting them
 * currently gets garbage fanned out — interop bug. Parser must mark
 * pending_error so the server tears down the connection.
 */
TEST(parser, reject_flag_compressed) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    /* Encode with flags=CMQ_FLAG_COMPRESSED (0x01) */
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(parser, reject_flag_checksum) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    /* Encode with flags=CMQ_FLAG_CHECKSUM (0x02) */
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH,
                                CMQ_FLAG_CHECKSUM, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(parser, reject_flag_combined_reserved) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    /* flags = 0x03 (both COMPRESSED|Checksum) — must also reject */
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH,
                                0x03, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(parser, accept_flag_headers) {
    /* Sanity: CMQ_FLAG_HEADERS (0x04) is already implemented and must
     * still parse without error. */
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH,
                                CMQ_FLAG_HEADERS, NULL, 0);
    ASSERT(n > 0);
    int rc = cmq_parser_feed(p, buf, n);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    cmq_parser_destroy(p);
}

TEST(parser, accept_flag_batch) {
    /* Sanity: CMQ_FLAG_BATCH (0x08) is already implemented and must
     * still parse without error. */
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_BATCH,
                                CMQ_FLAG_BATCH, NULL, 0);
    ASSERT(n > 0);
    int rc = cmq_parser_feed(p, buf, n);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    cmq_parser_destroy(p);
}

TEST_MAIN()
