/* v0.5.159: reload attaches txn log when create left it unset. */
#include "cmq_txn.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TXA_DIR "build-tdd/txa"

static int txa_apply(void *ctx, const char *subject, const uint8_t *data,
                     size_t len) {
    (void)ctx;
    (void)subject;
    (void)data;
    (void)len;
    return 0;
}

TEST(txa, apply) {
    (void)system("rm -rf " TXA_DIR " && mkdir -p " TXA_DIR);
    (void)system("rm -rf build-tdd/txa_other");
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_reload_attach_log(t, TXA_DIR), 0);
    ASSERT_EQ(cmq_txn_begin(t, 7), 0);
    ASSERT_EQ(cmq_txn_add(t, 7, "a.b", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_txn_commit(t, 7, txa_apply, NULL), 0);
    ASSERT_EQ(access(TXA_DIR "/cmq.txn", F_OK), 0);
    ASSERT_EQ(cmq_txn_reload_attach_log(t, "build-tdd/txa_other"), 0);
    ASSERT(access("build-tdd/txa_other/cmq.txn", F_OK) != 0);
    ASSERT_EQ(cmq_txn_was_committed(t, 7), 1);
    cmq_txn_destroy(t);
}

TEST(txa, omitted) {
    ASSERT_EQ(cmq_txn_reload_attach_log(NULL, NULL), 0);
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_reload_attach_log(t, NULL), 0);
    cmq_txn_destroy(t);
}

TEST(txa, empty) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_reload_attach_log(t, ""), 0);
    cmq_txn_destroy(t);
}

TEST(txa, reject) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT(cmq_txn_reload_attach_log(NULL, TXA_DIR) != 0);
    ASSERT(cmq_txn_reload_attach_log(t, "../evil") != 0);
    ASSERT(cmq_txn_reload_attach_log(t, "bad\\dir") != 0);
    ASSERT(access("../evil/cmq.txn", F_OK) != 0);
    cmq_txn_destroy(t);
}

TEST_MAIN()
