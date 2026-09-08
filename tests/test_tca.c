/* v0.5.163: reload creates the txn coordinator when create left it NULL. */
#include "cmq_txn.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TCA_DIR "build-tdd/tca"

static int tca_apply(void *ctx, const char *subject, const uint8_t *data,
                     size_t len) {
    (void)ctx;
    (void)subject;
    (void)data;
    (void)len;
    return 0;
}

TEST(tca, apply) {
    (void)system("rm -rf " TCA_DIR " && mkdir -p " TCA_DIR);
    cmq_txn_t *t = NULL;
    ASSERT(cmq_txn_reload_attach_log(t, TCA_DIR) != 0);
    ASSERT_EQ(cmq_txn_reload_attach(&t), 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_reload_attach_log(t, TCA_DIR), 0);
    ASSERT_EQ(cmq_txn_begin(t, 3), 0);
    ASSERT_EQ(cmq_txn_add(t, 3, "a.b", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_txn_commit(t, 3, tca_apply, NULL), 0);
    ASSERT_EQ(access(TCA_DIR "/cmq.txn", F_OK), 0);
    cmq_txn_t *same = t;
    ASSERT_EQ(cmq_txn_reload_attach(&t), 0);
    ASSERT(t == same);
    ASSERT_EQ(cmq_txn_was_committed(t, 3), 1);
    cmq_txn_destroy(t);
}

TEST(tca, omitted) {
    cmq_txn_t *t = NULL;
    ASSERT_EQ(cmq_txn_reload_attach(&t), 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_begin(t, 1), 0);
    cmq_txn_destroy(t);
}

TEST(tca, empty) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    cmq_txn_t *same = t;
    ASSERT_EQ(cmq_txn_reload_attach(&t), 0);
    ASSERT(t == same);
    cmq_txn_destroy(t);
}

TEST(tca, reject) {
    ASSERT(cmq_txn_reload_attach(NULL) != 0);
}

TEST_MAIN()
