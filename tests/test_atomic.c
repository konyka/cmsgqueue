#include "cmq_atomic.h"
#include "cmq_test.h"

TEST(atomic, load_store_u64) {
    cmq_atomic_u64 val = 0;
    cmq_atomic_store_u64(&val, 42, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(cmq_atomic_load_u64(&val, CMQ_ATOMIC_RELAXED), (cmq_u64_t)42);
}

TEST(atomic, fetch_add) {
    cmq_atomic_u32 val = 10;
    cmq_u32_t old = cmq_atomic_fetch_add_u32(&val, 5, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(old, (cmq_u32_t)10);
    ASSERT_EQ(cmq_atomic_load_u32(&val, CMQ_ATOMIC_RELAXED), (cmq_u32_t)15);
}

TEST(atomic, cas_u32) {
    cmq_atomic_u32 val = 100;
    cmq_u32_t expected = 100;
    ASSERT(cmq_atomic_cas_u32(&val, &expected, 200, CMQ_ATOMIC_ACQ_REL));
    ASSERT_EQ(cmq_atomic_load_u32(&val, CMQ_ATOMIC_RELAXED), (cmq_u32_t)200);
}

TEST(atomic, cas_fail) {
    cmq_atomic_u32 val = 100;
    cmq_u32_t expected = 99;
    ASSERT(!cmq_atomic_cas_u32(&val, &expected, 200, CMQ_ATOMIC_ACQ_REL));
    ASSERT_EQ(cmq_atomic_load_u32(&val, CMQ_ATOMIC_RELAXED), (cmq_u32_t)100);
}

TEST_MAIN()
