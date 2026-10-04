/* v0.5.102: disconnect span after graceful DISCONNECT. */
#include "cmq_test.h"
#include "cmq_otel.h"
#include <string.h>

TEST(otd, ok) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0xc4, sizeof(id));
    ASSERT_EQ(cmq_otel_on_disconnect(o, id, 1), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_DISCONNECT);
    ASSERT(memcmp(s.trace, id, 16) == 0);
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otd, skipped) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0xd5, sizeof(id));
    ASSERT_EQ(cmq_otel_on_disconnect(o, id, 0), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otd, isolated) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0xe6, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_CONNECT), 0);
    ASSERT_EQ(cmq_otel_on_disconnect(o, id, 1), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_CONNECT);
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_DISCONNECT);
    cmq_otel_destroy(o);
}

TEST(otd, reject) {
    uint8_t id[16];
    memset(id, 1, sizeof(id));
    ASSERT_EQ(cmq_otel_on_disconnect(NULL, id, 1), -1);
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_EQ(cmq_otel_on_disconnect(o, NULL, 1), -1);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST_MAIN()
