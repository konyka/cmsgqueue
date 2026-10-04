/* v0.5.100: request span after a successful local answer. */
#include "cmq_test.h"
#include "cmq_otel.h"
#include <string.h>

TEST(otr, ok) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0x6e, sizeof(id));
    ASSERT_EQ(cmq_otel_on_request(o, id, 1), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_REQUEST);
    ASSERT(memcmp(s.trace, id, 16) == 0);
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otr, skipped) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0x77, sizeof(id));
    ASSERT_EQ(cmq_otel_on_request(o, id, 0), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otr, isolated) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0x88, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_PUBLISH), 0);
    ASSERT_EQ(cmq_otel_on_request(o, id, 1), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_PUBLISH);
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_REQUEST);
    cmq_otel_destroy(o);
}

TEST(otr, reject) {
    uint8_t id[16];
    memset(id, 1, sizeof(id));
    ASSERT_EQ(cmq_otel_on_request(NULL, id, 1), -1);
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_EQ(cmq_otel_on_request(o, NULL, 1), -1);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST_MAIN()
