/* v0.5.96: COMPRESSED on PUBLISH (zstd, same bomb cap as BATCH). */
#include "cmq_test.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_compress.h"
#include <stdlib.h>
#include <string.h>

TEST(pubc, accept_publish) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, buf, n), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->hdr.op, CMQ_OP_PUBLISH);
    ASSERT_EQ(f->hdr.flags, CMQ_FLAG_COMPRESSED);
    cmq_parser_destroy(p);
}

TEST(pubc, reject_request) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_REQUEST,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(pubc, inflate_roundtrip) {
    uint8_t plain[6] = {0, 4, 't', 'e', 's', 't'};
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

TEST(pubc, inflate_reject) {
    uint8_t junk[16];
    memset(junk, 0x5a, sizeof(junk));
    uint8_t *out = NULL;
    size_t n = 0;
    ASSERT(cmq_inflate(junk, sizeof(junk), &out, &n) != 0);
    ASSERT(cmq_inflate(NULL, 1, &out, &n) != 0);
    ASSERT(cmq_inflate(junk, 0, &out, &n) != 0);
}

TEST_MAIN()
