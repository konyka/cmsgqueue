/* v0.5.165: reload creates the $JS manager when create left it NULL. */
#include "cmq_js.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JMA_DIR "build-tdd/jma"

TEST(jma, apply) {
    (void)system("rm -rf " JMA_DIR " && mkdir -p " JMA_DIR);
    cmq_js_t *j = NULL;
    ASSERT(cmq_js_reload_attach_persist(j, JMA_DIR) != 0);
    ASSERT_EQ(cmq_js_reload_attach(&j), 0);
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_reload_attach_persist(j, JMA_DIR), 0);
    ASSERT_EQ(cmq_js_publish(j, "$JS.ev", (const uint8_t *)"v1", 2), 1);
    ASSERT_EQ(access(JMA_DIR "/js/ev.last", F_OK), 0);
    cmq_js_t *same = j;
    ASSERT_EQ(cmq_js_reload_attach(&j), 0);
    ASSERT(j == same);
    uint8_t out[8];
    size_t n = 0;
    ASSERT_EQ(cmq_js_request(j, "$JS.ev", out, sizeof(out), &n), 1);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(memcmp(out, "v1", 2) == 0);
    cmq_js_destroy(j);
}

TEST(jma, omitted) {
    cmq_js_t *j = NULL;
    ASSERT_EQ(cmq_js_reload_attach(&j), 0);
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(cmq_js_publish(j, "$JS.mem", (const uint8_t *)"x", 1), 1);
    cmq_js_destroy(j);
}

TEST(jma, empty) {
    cmq_js_t *j = cmq_js_create();
    ASSERT_NOT_NULL(j);
    cmq_js_t *same = j;
    ASSERT_EQ(cmq_js_reload_attach(&j), 0);
    ASSERT(j == same);
    cmq_js_destroy(j);
}

TEST(jma, reject) {
    ASSERT(cmq_js_reload_attach(NULL) != 0);
}

TEST_MAIN()
