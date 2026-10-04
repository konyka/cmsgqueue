/* v0.5.61: D1 phase 1 OTel span ring. */
#include "cmq_test.h"
#include "cmq_otel.h"
#include <string.h>
#include <time.h>

TEST(otel, offer_poll) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0xab, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_PUBLISH), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_PUBLISH);
    ASSERT(memcmp(s.trace, id, 16) == 0);
    ASSERT_EQ(cmq_otel_poll(o, &s), 0);
    cmq_otel_destroy(o);
}

TEST(otel, drop_newest) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 1, sizeof(id));
    for (int i = 0; i < CMQ_OTEL_RING; i++)
        ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_CONNECT), 0);
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_PUBLISH), 1);
    ASSERT_EQ(cmq_otel_dropped(o), (uint64_t)1);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_CONNECT);
    cmq_otel_destroy(o);
}

TEST(otel, isolated_kinds) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t a[16], b[16];
    memset(a, 0x11, 16);
    memset(b, 0x22, 16);
    ASSERT_EQ(cmq_otel_offer(o, a, CMQ_OTEL_KIND_PUBLISH), 0);
    ASSERT_EQ(cmq_otel_offer(o, b, CMQ_OTEL_KIND_CONSUME), 0);
    cmq_otel_span_t s;
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_PUBLISH);
    ASSERT_EQ(cmq_otel_poll(o, &s), 1);
    ASSERT_EQ(s.kind, (uint8_t)CMQ_OTEL_KIND_CONSUME);
    cmq_otel_destroy(o);
}

static uint8_t g_export_kind;

static void test_export(void *ctx, const cmq_otel_span_t *span) {
    int *n = ctx;
    if (n) (*n)++;
    if (span) g_export_kind = span->kind;
}

TEST(otel, sidecar_drains) {
    cmq_otel_t *o = cmq_otel_create();
    int n = 0;
    cmq_otel_set_export(o, test_export, &n);
    ASSERT_EQ(cmq_otel_start(o), 0);
    uint8_t id[16];
    memset(id, 0xcd, 16);
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_CONNECT), 0);
    for (int i = 0; i < 50 && n == 0; i++) {
        struct timespec ts = {0, 2000000L};
        nanosleep(&ts, NULL);
    }
    cmq_otel_stop(o);
    ASSERT(n >= 1);
    ASSERT_EQ(g_export_kind, (uint8_t)CMQ_OTEL_KIND_CONNECT);
    ASSERT(cmq_otel_exported(o) >= 1);
    cmq_otel_destroy(o);
}

TEST(otel, reject_bad) {
    cmq_otel_t *o = cmq_otel_create();
    uint8_t id[16];
    memset(id, 0, 16);
    ASSERT(cmq_otel_offer(NULL, id, 1) != 0);
    ASSERT(cmq_otel_offer(o, NULL, 1) != 0);
    ASSERT(cmq_otel_offer(o, id, 0) != 0);
    ASSERT(cmq_otel_offer(o, id, 99) != 0);
    cmq_otel_destroy(o);
}

TEST_MAIN()
