/* v0.5.164: reload creates the KV manager when create left it NULL. */
#include "cmq_kvb.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KBA_DIR "build-tdd/kba"

TEST(kba, apply) {
    (void)system("rm -rf " KBA_DIR " && mkdir -p " KBA_DIR);
    cmq_kvb_t *b = NULL;
    ASSERT(cmq_kvb_reload_attach_persist(b, KBA_DIR) != 0);
    ASSERT_EQ(cmq_kvb_reload_attach(&b), 0);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, KBA_DIR), 0);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k1", (const uint8_t *)"v1", 2), 1);
    ASSERT_EQ(access(KBA_DIR "/kv_acc.data", F_OK), 0);
    cmq_kvb_t *same = b;
    ASSERT_EQ(cmq_kvb_reload_attach(&b), 0);
    ASSERT(b == same);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_get(b, "$KV.acc.k1", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(memcmp(out, "v1", 2) == 0);
    cmq_kvb_destroy(b);
}

TEST(kba, omitted) {
    cmq_kvb_t *b = NULL;
    ASSERT_EQ(cmq_kvb_reload_attach(&b), 0);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.mem.k", (const uint8_t *)"x", 1), 1);
    cmq_kvb_destroy(b);
}

TEST(kba, empty) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    cmq_kvb_t *same = b;
    ASSERT_EQ(cmq_kvb_reload_attach(&b), 0);
    ASSERT(b == same);
    cmq_kvb_destroy(b);
}

TEST(kba, reject) {
    ASSERT(cmq_kvb_reload_attach(NULL) != 0);
}

TEST_MAIN()
