/* v0.5.70: D4 REQUEST-get for $KV / $OBJ. */
#include "cmq_test.h"
#include "cmq_kvb.h"
#include "cmq_obj.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define ODIR "/tmp/cmq_kvreq"

TEST(kvreq, kv_hit_miss) {
    cmq_kvb_t *b = cmq_kvb_create();
    ASSERT_EQ(cmq_kvb_publish(b, "$KV.acc.k", (const uint8_t *)"z", 1), 1);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_request(b, "$KV.acc.k", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], (uint8_t)'z');
    ASSERT_EQ(cmq_kvb_request(b, "$KV.acc.missing", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    cmq_kvb_destroy(b);
}

TEST(kvreq, kv_not) {
    cmq_kvb_t *b = cmq_kvb_create();
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_kvb_request(b, "plain", out, sizeof(out), &n), -1);
    cmq_kvb_destroy(b);
}

TEST(kvreq, obj_hit_miss) {
    mkdir(ODIR, 0755);
    remove(ODIR "/blob");
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_NOT_NULL(o);
    ASSERT_EQ(cmq_obj_publish(o, "$OBJ.blob", (const uint8_t *)"ab", 2), 1);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_EQ(cmq_obj_request(o, "$OBJ.blob", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(cmq_obj_request(o, "$OBJ.nope", out, sizeof(out), &n), 0);
    ASSERT_EQ(n, (size_t)0);
    cmq_obj_destroy(o);
    remove(ODIR "/blob");
}

TEST(kvreq, reject) {
    cmq_kvb_t *b = cmq_kvb_create();
    uint8_t out[8];
    size_t n = 0;
    ASSERT(cmq_kvb_request(NULL, "$KV.a.b", out, sizeof(out), &n) != 0);
    ASSERT(cmq_obj_request(NULL, "$OBJ.x", out, sizeof(out), &n) != 0);
    ASSERT(cmq_kvb_request(b, NULL, out, sizeof(out), &n) != 0);
    ASSERT(cmq_kvb_request(b, "$KV.bad", out, sizeof(out), &n) != 0);
    cmq_kvb_destroy(b);
}

TEST_MAIN()
