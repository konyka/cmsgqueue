/* v0.5.99: COMPRESSED on RESPONSE. */
#include "cmq_test.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_compress.h"
#include <stdlib.h>
#include <string.h>

TEST(resc, accept_response) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_RESPONSE,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, buf, n), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->hdr.op, CMQ_OP_RESPONSE);
    cmq_parser_destroy(p);
}

TEST(resc, reject_subscribe) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_SUBSCRIBE,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(resc, inflate_roundtrip) {
    uint8_t plain[] = {0, 7, '_', 'I', 'N', 'B', 'O', 'X', '.', 'x'};
    size_t cap = cmq_compress_bound(sizeof(plain));
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t elen = cmq_compress(plain, sizeof(plain), enc, cap);
    ASSERT(elen > 0);
    uint8_t *out = NULL;
    size_t n = 0;
    ASSERT_EQ(cmq_inflate(enc, (size_t)elen, &out, &n), 0);
    ASSERT_EQ(n, sizeof(plain));
    ASSERT(memcmp(out, plain, sizeof(plain)) == 0);
    free(out);
    free(enc);
}

TEST(resc, reject) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNECT,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
    uint8_t *out = NULL;
    size_t ln = 0;
    ASSERT(cmq_inflate(NULL, 1, &out, &ln) != 0);
}

TEST_MAIN()
