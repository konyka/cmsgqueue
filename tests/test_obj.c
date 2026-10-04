/* v0.5.59: named object store (D4 phase 3). */
#include "cmq_test.h"
#include "cmq_obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define ODIR "/tmp/cmq_obj"

static void wipe(void) {
    remove(ODIR "/alpha");
    remove(ODIR "/alpha.tmp");
    remove(ODIR "/beta");
    remove(ODIR "/beta.tmp");
    remove(ODIR "/gone");
    remove(ODIR "/gone.tmp");
}

TEST(obj, put_get_overwrite) {
    mkdir(ODIR, 0755);
    wipe();
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_NOT_NULL(o);
    ASSERT_EQ(cmq_obj_put(o, "alpha", (const uint8_t *)"one", 3), 0);
    uint8_t buf[16];
    size_t n = 0;
    ASSERT_EQ(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(n, (size_t)3);
    ASSERT(memcmp(buf, "one", 3) == 0);
    ASSERT_EQ(cmq_obj_put(o, "alpha", (const uint8_t *)"two!", 4), 0);
    ASSERT_EQ(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(n, (size_t)4);
    ASSERT(memcmp(buf, "two!", 4) == 0);
    cmq_obj_destroy(o);
    o = cmq_obj_create(ODIR);
    ASSERT_EQ(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n), 0);
    ASSERT(memcmp(buf, "two!", 4) == 0);
    cmq_obj_destroy(o);
    wipe();
}

TEST(obj, del_and_isolated) {
    mkdir(ODIR, 0755);
    wipe();
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_EQ(cmq_obj_put(o, "alpha", (const uint8_t *)"a", 1), 0);
    ASSERT_EQ(cmq_obj_put(o, "beta", (const uint8_t *)"b", 1), 0);
    ASSERT_EQ(cmq_obj_del(o, "alpha"), 0);
    uint8_t buf[8];
    size_t n = 0;
    ASSERT(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n) != 0);
    ASSERT_EQ(cmq_obj_get(o, "beta", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(buf[0], (uint8_t)'b');
    ASSERT(cmq_obj_del(o, "alpha") != 0);
    cmq_obj_destroy(o);
    wipe();
}

TEST(obj, reject_unsafe) {
    mkdir(ODIR, 0755);
    wipe();
    ASSERT(cmq_obj_create("/tmp/../etc") == NULL);
    ASSERT(cmq_obj_create(NULL) == NULL);
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_NOT_NULL(o);
    ASSERT(cmq_obj_put(o, "bad name", (const uint8_t *)"x", 1) != 0);
    ASSERT(cmq_obj_put(o, "a/b", (const uint8_t *)"x", 1) != 0);
    ASSERT(cmq_obj_put(o, "..", (const uint8_t *)"x", 1) != 0);
    ASSERT_EQ(cmq_obj_put(o, "ok_1", (const uint8_t *)"x", 1), 0);
    uint8_t *big = malloc(CMQ_OBJ_VAL_MAX + 1);
    ASSERT_NOT_NULL(big);
    memset(big, 'z', CMQ_OBJ_VAL_MAX + 1);
    ASSERT_EQ(cmq_obj_put(o, "ok_1", big, CMQ_OBJ_VAL_MAX + 1), -2);
    free(big);
    cmq_obj_destroy(o);
    remove(ODIR "/ok_1");
    wipe();
}

TEST_MAIN()
