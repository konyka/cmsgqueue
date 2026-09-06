/* v0.5.92: connect span after CONNACK 0. */
#include "cmq_test.h"
#include "cmq_otel.h"
#include <string.h>

TEST(otn, ok) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0x3c, sizeof(id));
    ASSERT_EQ(cmq_otel_on_connect(o, id, 1), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_CONNECT);
    ASSERT(memcmp(s.trace, id, 16) == 0);
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otn, skipped) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0x44, sizeof(id));
    ASSERT_EQ(cmq_otel_on_connect(o, id, 0), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otn, isolated) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0x55, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_PUBLISH), 0);
    ASSERT_EQ(cmq_otel_on_connect(o, id, 1), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_PUBLISH);
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_CONNECT);
    cmq_otel_destroy(o);
}

TEST(otn, reject) {
    uint8_t id[16];
    memset(id, 1, sizeof(id));
    ASSERT_EQ(cmq_otel_on_connect(NULL, id, 1), -1);
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_EQ(cmq_otel_on_connect(o, NULL, 1), -1);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST_MAIN()
