/* v0.5.68: D4 $OBJ.<name> PUBLISH path. */
#include "cmq_test.h"
#include "cmq_obj.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define ODIR "/tmp/cmq_objp"

static void wipe(void) {
    remove(ODIR "/alpha");
    remove(ODIR "/alpha.tmp");
    remove(ODIR "/beta");
    remove(ODIR "/beta.tmp");
}

TEST(objp, parse) {
    char n[CMQ_OBJ_NAME_MAX];
    ASSERT_EQ(cmq_obj_parse("$OBJ.alpha", n, sizeof(n)), 0);
    ASSERT_STR_EQ(n, "alpha");
    ASSERT_EQ(cmq_obj_parse("foo.bar", n, sizeof(n)), -1);
    ASSERT_EQ(cmq_obj_parse("$OBJ.", n, sizeof(n)), -2);
    ASSERT_EQ(cmq_obj_parse("$OBJ.a/b", n, sizeof(n)), -2);
}

TEST(objp, put_get_del) {
    mkdir(ODIR, 0755);
    wipe();
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_NOT_NULL(o);
    ASSERT_EQ(cmq_obj_publish(o, "$OBJ.alpha", (const uint8_t *)"hi", 2), 1);
    uint8_t buf[8];
    size_t n = 0;
    ASSERT_EQ(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(cmq_obj_publish(o, "$OBJ.alpha", NULL, 0), 1);
    ASSERT(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n) != 0);
    cmq_obj_destroy(o);
    wipe();
}

TEST(objp, isolate) {
    mkdir(ODIR, 0755);
    wipe();
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_EQ(cmq_obj_publish(o, "$OBJ.alpha", (const uint8_t *)"a", 1), 1);
    ASSERT_EQ(cmq_obj_publish(o, "$OBJ.beta", (const uint8_t *)"b", 1), 1);
    uint8_t buf[8];
    size_t n = 0;
    ASSERT_EQ(cmq_obj_get(o, "alpha", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(buf[0], (uint8_t)'a');
    ASSERT_EQ(cmq_obj_get(o, "beta", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(buf[0], (uint8_t)'b');
    cmq_obj_destroy(o);
    wipe();
}

TEST(objp, reject) {
    mkdir(ODIR, 0755);
    wipe();
    cmq_obj_t *o = cmq_obj_create(ODIR);
    ASSERT_EQ(cmq_obj_publish(o, "plain", (const uint8_t *)"x", 1), 0);
    ASSERT(cmq_obj_publish(o, "$OBJ.", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_obj_publish(o, "$OBJ.bad name", (const uint8_t *)"x", 1) < 0);
    ASSERT(cmq_obj_parse(NULL, NULL, 0) != 0);
    cmq_obj_destroy(o);
    wipe();
}

TEST_MAIN()
