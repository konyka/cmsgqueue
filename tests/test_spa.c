/* v0.5.157: reload attaches cmq-subs.wal when create left persist NULL. */
#include "cmq_sublist_persist.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPA_DIR "build-tdd/spa"

TEST(spa, apply) {
    (void)system("rm -rf " SPA_DIR " && mkdir -p " SPA_DIR);
    cmq_sublist_persist_t *p = NULL;
    ASSERT_EQ(cmq_sublist_persist_reload_attach(&p, SPA_DIR), 0);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 1, "a.b", "acc"), 0);
    cmq_sublist_persist_t *same = p;
    ASSERT_EQ(cmq_sublist_persist_reload_attach(&p, "build-tdd/spa_other"), 0);
    ASSERT(p == same);
    cmq_sublist_persist_close(p);
}

TEST(spa, omitted) {
    cmq_sublist_persist_t *p = NULL;
    ASSERT_EQ(cmq_sublist_persist_reload_attach(&p, NULL), 0);
    ASSERT(p == NULL);
}

TEST(spa, empty) {
    cmq_sublist_persist_t *p = NULL;
    ASSERT_EQ(cmq_sublist_persist_reload_attach(&p, ""), 0);
    ASSERT(p == NULL);
}

TEST(spa, reject) {
    cmq_sublist_persist_t *p = NULL;
    ASSERT(cmq_sublist_persist_reload_attach(NULL, SPA_DIR) != 0);
    ASSERT(cmq_sublist_persist_reload_attach(&p, "../evil") != 0);
    ASSERT(p == NULL);
    ASSERT(cmq_sublist_persist_reload_attach(&p, "bad\\dir") != 0);
    ASSERT(p == NULL);
}

TEST_MAIN()
