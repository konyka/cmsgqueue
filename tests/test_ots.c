/* v0.5.101: response span after a successful local deliver. */
#include "cmq_test.h"
#include "cmq_otel.h"
#include <string.h>

TEST(ots, ok) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0x91, sizeof(id));
    ASSERT_EQ(cmq_otel_on_response(o, id, 1), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_RESPONSE);
    ASSERT(memcmp(s.trace, id, 16) == 0);
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(ots, skipped) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0xa2, sizeof(id));
    ASSERT_EQ(cmq_otel_on_response(o, id, 0), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(ots, isolated) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0xb3, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_REQUEST), 0);
    ASSERT_EQ(cmq_otel_on_response(o, id, 1), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_REQUEST);
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_RESPONSE);
    cmq_otel_destroy(o);
}

TEST(ots, reject) {
    uint8_t id[16];
    memset(id, 1, sizeof(id));
    ASSERT_EQ(cmq_otel_on_response(NULL, id, 1), -1);
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_EQ(cmq_otel_on_response(o, NULL, 1), -1);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST_MAIN()
