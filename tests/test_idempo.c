/* v0.5.55: D5 phase-1 idempotent publish window. */
#include "cmq_test.h"
#include "cmq_idempo.h"
#include <string.h>

TEST(idempo, parse_roundtrip) {
    uint8_t buf[16];
    size_t n = 0;
    ASSERT_EQ(cmq_idempo_encode(buf, sizeof(buf), 0xAABBCCDDu, 42, &n), 0);
    ASSERT_EQ(n, (size_t)CMQ_IDEMPO_HDR_LEN);
    uint32_t pid = 0;
    uint64_t seq = 0;
    ASSERT_EQ(cmq_idempo_parse(buf, n, &pid, &seq), 0);
    ASSERT_EQ(pid, 0xAABBCCDDu);
    ASSERT_EQ(seq, (uint64_t)42);
}

TEST(idempo, parse_rejects) {
    uint8_t buf[16];
    size_t n = 0;
    ASSERT_EQ(cmq_idempo_encode(buf, sizeof(buf), 0, 1, &n), -1);
    memset(buf, 0, sizeof(buf));
    uint32_t pid = 1;
    uint64_t seq = 1;
    ASSERT_EQ(cmq_idempo_parse(buf, 15, &pid, &seq), -1);
    ASSERT_EQ(cmq_idempo_parse((const uint8_t *)"XXXX............", 16,
                                &pid, &seq),
              -1);
}

TEST(idempo, first_then_dup) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 1);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 0);
    ASSERT_EQ(cmq_idempo_check(t, 7, 1), 0);
    cmq_idempo_destroy(t);
}

TEST(idempo, advance_old_is_dup) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_EQ(cmq_idempo_check(t, 1, 10), 1);
    ASSERT_EQ(cmq_idempo_check(t, 1, 11), 1);
    ASSERT_EQ(cmq_idempo_check(t, 1, 10), 0);
    ASSERT_EQ(cmq_idempo_check(t, 1, 11), 0);
    cmq_idempo_destroy(t);
}

TEST(idempo, ooo_in_window) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_EQ(cmq_idempo_check(t, 3, 5), 1);
    ASSERT_EQ(cmq_idempo_check(t, 3, 3), 1);
    ASSERT_EQ(cmq_idempo_check(t, 3, 3), 0);
    ASSERT_EQ(cmq_idempo_check(t, 3, 4), 1);
    cmq_idempo_destroy(t);
}

TEST(idempo, too_old_is_dup) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_EQ(cmq_idempo_check(t, 9, 100), 1);
    ASSERT_EQ(cmq_idempo_check(t, 9, 20), 0);
    cmq_idempo_destroy(t);
}

TEST(idempo, isolated_pids) {
    cmq_idempo_t *t = cmq_idempo_create();
    ASSERT_EQ(cmq_idempo_check(t, 1, 1), 1);
    ASSERT_EQ(cmq_idempo_check(t, 2, 1), 1);
    ASSERT_EQ(cmq_idempo_check(t, 1, 1), 0);
    ASSERT_EQ(cmq_idempo_check(t, 2, 2), 1);
    cmq_idempo_destroy(t);
}

TEST(idempo, table_full) {
    cmq_idempo_t *t = cmq_idempo_create();
    for (uint32_t i = 1; i <= (uint32_t)CMQ_IDEMPO_PIDS; i++)
        ASSERT_EQ(cmq_idempo_check(t, i, 1), 1);
    ASSERT_EQ(cmq_idempo_check(t, 3000, 1), -2);
    ASSERT_EQ(cmq_idempo_check(t, 1, 2), 1);
    cmq_idempo_destroy(t);
}

TEST_MAIN()
