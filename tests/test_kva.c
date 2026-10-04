/* v0.5.160: reload attaches KV persist when create left it unset. */
#include "cmq_kvb.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KVA_DIR "build-tdd/kva"

TEST(kva, apply) {
    (void)system("rm -rf " KVA_DIR " && mkdir -p " KVA_DIR);
    (void)system("rm -rf build-tdd/kva_other");
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, KVA_DIR), 0);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k1", (const uint8_t *)"v1", 2), 1);
    ASSERT_EQ(access(KVA_DIR "/kv_acc.data", F_OK), 0);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, "build-tdd/kva_other"), 0);
    ASSERT(access("build-tdd/kva_other/kv_acc.data", F_OK) != 0);
    cmq_kvb_destroy(b);
    b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, KVA_DIR), 0);
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k2", (const uint8_t *)"z", 1), 1);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_get(b, "$KV.acc.k1", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(memcmp(out, "v1", 2) == 0);
    cmq_kvb_destroy(b);
}

TEST(kva, omitted) {
    ASSERT_EQ(cmq_kvb_reload_attach_persist(NULL, NULL), 0);
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, NULL), 0);
    cmq_kvb_destroy(b);
}

TEST(kva, empty) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(cmq_kvb_reload_attach_persist(b, ""), 0);
    cmq_kvb_destroy(b);
}

TEST(kva, reject) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_NOT_NULL(b);
    ASSERT(cmq_kvb_reload_attach_persist(NULL, KVA_DIR) != 0);
    ASSERT(cmq_kvb_reload_attach_persist(b, "../evil") != 0);
    ASSERT(cmq_kvb_reload_attach_persist(b, "bad\\dir") != 0);
    ASSERT(access("../evil/kv_acc.data", F_OK) != 0);
    cmq_kvb_destroy(b);
}

TEST_MAIN()
