/* v0.5.67: D4 KV bucket PUBLISH path. */
#include "cmq_test.h"
#include "cmq_kvb.h"
#include <string.h>

TEST(kvb, parse) {
    char b[CMQ_KVB_BUCKET_MAX], k[256];
    ASSERT_EQ(cmq_kvb_parse("$KV.orders.user.42", b, sizeof(b), k, sizeof(k)),
              0);
    ASSERT_STR_EQ(b, "orders");
    ASSERT_STR_EQ(k, "user.42");
    ASSERT_EQ(cmq_kvb_parse("foo.bar", b, sizeof(b), k, sizeof(k)), -1);
    ASSERT_EQ(cmq_kvb_parse("$KV.bad", b, sizeof(b), k, sizeof(k)), -2);
    ASSERT_EQ(cmq_kvb_parse("$KV..x", b, sizeof(b), k, sizeof(k)), -2);
}

TEST(kvb, put_get_del) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k1", (const uint8_t *)"v1", 2), 1);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_get(b, "$KV.acc.k1", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(memcmp(out, "v1", 2) == 0);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k1", NULL, 0), 1);
    ASSERT(cmq_kvb_get(b, "$KV.acc.k1", out, sizeof(out), &n) != 0);
    cmq_kvb_destroy(b);
}

TEST(kvb, isolate) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.a.k", (const uint8_t *)"1", 1), 1);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.b.k", (const uint8_t *)"2", 1), 1);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_get(b, "$KV.a.k", out, sizeof(out), &n), 0);
    ASSERT_EQ(out[0], (uint8_t)'1');
    ASSERT_EQ(cmq_kvb_get(b, "$KV.b.k", out, sizeof(out), &n), 0);
    ASSERT_EQ(out[0], (uint8_t)'2');
    cmq_kvb_destroy(b);
}

TEST(kvb, reject) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_EQ(cmq_kvb_publish(b, "plain", (const uint8_t *)"x", 1), 0);
    ASSERT(cmq_kvb_publish(b, "$KV.no", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_kvb_publish(b, "$KV.b.bad key", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_kvb_parse(NULL, NULL, 0, NULL, 0) != 0);
    cmq_kvb_destroy(b);
}

TEST_MAIN()
