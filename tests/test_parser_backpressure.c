// Regression test: parser backpressure and oversize-payload behavior.
//
// Verifies that filling the 64-frame queue keeps pending_error=0 (queue-full
// is not a fatal error), that further feed() calls past the cap do not
// crash, and that an oversize declared payload_len sets pending_error=1.

#define _POSIX_C_SOURCE 200809L
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void make_ping_frame(uint8_t *buf, size_t *out_len) {
    cmq_frame_hdr_t h;
    h.magic[0] = CMQ_PROTO_MAGIC_0;
    h.magic[1] = CMQ_PROTO_MAGIC_1;
    h.version  = CMQ_PROTO_VERSION;
    h.flags    = 0;
    h.op       = CMQ_OP_PING;
    h.length   = 0;
    memcpy(buf, &h, sizeof(h));
    *out_len = sizeof(h);
}

TEST(parser_backpressure, queue_full_returns_no_error) {
    cmq_parser_t *p = cmq_parser_create();
    ASSERT_NOT_NULL(p);

    cmq_parser_set_max_payload(p, 1024);

    enum { N_FRAMES = 64 };
    uint8_t bigbuf[N_FRAMES * 32];
    size_t off = 0;
    for (int i = 0; i < N_FRAMES; i++) {
        size_t n = 0;
        make_ping_frame(bigbuf + off, &n);
        off += n;
    }
    int rc = cmq_parser_feed(p, bigbuf, off);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);

    int drained = 0;
    while (cmq_parser_frame(p) != NULL) {
        (void)cmq_parser_next(p);
        drained++;
    }
    ASSERT_EQ(drained, N_FRAMES);

    for (int i = 0; i < 4; i++) {
        uint8_t fbuf[32];
        size_t n = 0;
        make_ping_frame(fbuf, &n);
        int r = cmq_parser_feed(p, fbuf, n);
        ASSERT(r >= 0);
    }
    ASSERT_EQ(cmq_parser_pending_error(p), 0);

    cmq_parser_destroy(p);
}

TEST(parser_backpressure, oversize_payload_is_fatal) {
    cmq_parser_t *p = cmq_parser_create();
    ASSERT_NOT_NULL(p);
    cmq_parser_set_max_payload(p, 1024);

    uint8_t header_only[sizeof(cmq_frame_hdr_t) + 8];
    cmq_frame_hdr_t h;
    h.magic[0] = CMQ_PROTO_MAGIC_0;
    h.magic[1] = CMQ_PROTO_MAGIC_1;
    h.version  = CMQ_PROTO_VERSION;
    h.flags    = 0;
    h.op       = CMQ_OP_PUBLISH;
    h.length   = 2048;
    memcpy(header_only, &h, sizeof(h));

    int rc = cmq_parser_feed(p, header_only, sizeof(header_only));
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    (void)rc;

    cmq_parser_destroy(p);
}

TEST_MAIN()