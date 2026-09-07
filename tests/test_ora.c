/* v0.5.158: reload attaches object store when create left obj NULL. */
#include "cmq_obj.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ORA_DIR "build-tdd/ora"

TEST(ora, apply) {
    (void)system("rm -rf " ORA_DIR " && mkdir -p " ORA_DIR);
    cmq_obj_t *o = NULL;
    ASSERT_EQ(cmq_obj_reload_attach(&o, ORA_DIR), 0);
    ASSERT_NOT_NULL(o);
    ASSERT_EQ(cmq_obj_put(o, "k", (const uint8_t *)"v", 1), 0);
    uint8_t buf[8];
    size_t n = 0;
    ASSERT_EQ(cmq_obj_get(o, "k", buf, sizeof(buf), &n), 0);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(buf[0], (uint8_t)'v');
    cmq_obj_t *same = o;
    ASSERT_EQ(cmq_obj_reload_attach(&o, "build-tdd/ora_other"), 0);
    ASSERT(o == same);
    cmq_obj_destroy(o);
}

TEST(ora, omitted) {
    cmq_obj_t *o = NULL;
    ASSERT_EQ(cmq_obj_reload_attach(&o, NULL), 0);
    ASSERT(o == NULL);
}

TEST(ora, empty) {
    cmq_obj_t *o = NULL;
    ASSERT_EQ(cmq_obj_reload_attach(&o, ""), 0);
    ASSERT(o == NULL);
}

TEST(ora, reject) {
    cmq_obj_t *o = NULL;
    ASSERT(cmq_obj_reload_attach(NULL, ORA_DIR) != 0);
    ASSERT(cmq_obj_reload_attach(&o, "../evil") != 0);
    ASSERT(o == NULL);
    ASSERT(cmq_obj_reload_attach(&o, "bad\\dir") != 0);
    ASSERT(o == NULL);
}

TEST_MAIN()
