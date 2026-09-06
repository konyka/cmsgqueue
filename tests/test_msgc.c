/* v0.5.97: COMPRESSED on inbound MESSAGE. */
#include "cmq_test.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_compress.h"
#include <stdlib.h>
#include <string.h>

TEST(msgc, accept_message) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_MESSAGE,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, buf, n), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->hdr.op, CMQ_OP_MESSAGE);
    cmq_parser_destroy(p);
}

TEST(msgc, reject_response) {
    cmq_parser_t *p = cmq_parser_create();
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_RESPONSE,
                                CMQ_FLAG_COMPRESSED, NULL, 0);
    ASSERT(n > 0);
    (void)cmq_parser_feed(p, buf, n);
    ASSERT_EQ(cmq_parser_pending_error(p), 1);
    cmq_parser_destroy(p);
}

TEST(msgc, to_publish) {
    /* sub_id=1 subject=ab headers="" body=xy */
    uint8_t msg[] = {
        0, 0, 0, 1,
        0, 2, 'a', 'b',
        0, 0,
        0, 0, 0, 2, 'x', 'y'
    };
    uint8_t *out = NULL;
    size_t n = 0;
    uint8_t flags = 0xff;
    ASSERT_EQ(cmq_message_to_publish(msg, sizeof(msg), &out, &n, &flags), 0);
    ASSERT_EQ(flags, (uint8_t)0);
    ASSERT_EQ(n, (size_t)8);
    ASSERT_EQ(out[0], (uint8_t)0);
    ASSERT_EQ(out[1], (uint8_t)2);
    ASSERT_EQ(out[2], (uint8_t)'a');
    ASSERT_EQ(out[3], (uint8_t)'b');
    ASSERT_EQ(out[4], (uint8_t)0);
    ASSERT_EQ(out[5], (uint8_t)0);
    ASSERT_EQ(out[6], (uint8_t)'x');
    ASSERT_EQ(out[7], (uint8_t)'y');
    free(out);
}

TEST(msgc, reject) {
    uint8_t *out = NULL;
    size_t n = 0;
    uint8_t flags = 0;
    uint8_t shortb[4] = {0};
    ASSERT(cmq_message_to_publish(NULL, 0, &out, &n, &flags) != 0);
    ASSERT(cmq_message_to_publish(shortb, sizeof(shortb), &out, &n, &flags)
           != 0);
    ASSERT(cmq_message_to_publish(shortb, 0, &out, &n, NULL) != 0);
}

TEST_MAIN()
