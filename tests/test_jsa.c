/* v0.5.161: reload attaches $JS persist when create left it unset. */
#include "cmq_js.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JSA_DIR "build-tdd/jsa"

TEST(jsa, apply) {
    (void)system("rm -rf " JSA_DIR " && mkdir -p " JSA_DIR);
    (void)system("rm -rf build-tdd/jsa_other");
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, JSA_DIR), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"v1", 2), 1);
    ASSERT_EQ(access(JSA_DIR "/js/ev.last", F_OK), 0);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, "build-tdd/jsa_other"), 0);
    ASSERT(access("build-tdd/jsa_other/js/ev.last", F_OK) != 0);
    cmq_js_destroy(j);
    j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, JSA_DIR), 0);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "$JS.ev", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(memcmp(out, "v1", 2) == 0);
    cmq_js_destroy(j);
}

TEST(jsa, omitted) {
    ASSERT_EQ(cmq_js_reload_attach_persist(NULL, NULL), 0);
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, NULL), 0);
    cmq_js_destroy(j);
}

TEST(jsa, empty) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, ""), 0);
    cmq_js_destroy(j);
}

TEST(jsa, reject) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    ASSERT(cmq_js_reload_attach_persist(NULL, JSA_DIR) != 0);
    ASSERT(cmq_js_reload_attach_persist(j, "../evil") != 0);
    ASSERT(cmq_js_reload_attach_persist(j, "bad\\dir") != 0);
    ASSERT(access("../evil/js/ev.last", F_OK) != 0);
    cmq_js_destroy(j);
}

TEST_MAIN()
