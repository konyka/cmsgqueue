/* v0.5.167: reload creates the idempo window when create left it NULL. */
#include "cmq_idempo.h"
#include "cmq_test.h"

TEST(ida, apply) {
    cmq_idempo_t *t = NULL;
    ASSERT_EQ(cmq_idempo_reload_attach(&t), 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 1);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 0);
    cmq_idempo_t *same = t;
    ASSERT_EQ(cmq_idempo_reload_attach(&t), 0);
    ASSERT(t == same);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 0);
    cmq_idempo_destroy(t);
}

TEST(ida, omitted) {
    cmq_idempo_t *t = NULL;
    ASSERT_EQ(cmq_idempo_reload_attach(&t), 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_idempo_check(t, 1, 1), 1);
    cmq_idempo_destroy(t);
}

TEST(ida, empty) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_idempo_check(t, 3, 9), 1);
    cmq_idempo_t *same = t;
    ASSERT_EQ(cmq_idempo_reload_attach(&t), 0);
    ASSERT(t == same);
    ASSERT_EQ(cmq_idempo_check(t, 3, 9), 0);
    cmq_idempo_destroy(t);
}

TEST(ida, reject) {
    ASSERT(cmq_idempo_reload_attach(NULL) != 0);
}

TEST_MAIN()
