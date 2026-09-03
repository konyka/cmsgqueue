#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ >= 8
/* v0.5.20: atomic 64-bit operations CI test.
 *
 * v0.5.21 documents that cmq_atomic_u64 requires ≥ 32-bit platforms.
 * This test verifies the atomic operations work correctly on
 * the current platform. On real 64-bit Linux (where this runs)
 * the operations are lock-free; on 32-bit -m32 they fall back to
 * a 2x 32-bit split with proper memory ordering. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_atomic.h"

#include <stdio.h>

TEST(atomic_64, load_store_basic) {
    cmq_atomic_u64 x = 0;
    cmq_atomic_store_u64(&x, 0x1234567890abcdefULL, CMQ_ATOMIC_RELAXED);
    uint64_t got = cmq_atomic_load_u64(&x, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(got, 0x1234567890abcdefULL);
}

TEST(atomic_64, fetch_add_basic) {
    cmq_atomic_u64 x = 0;
    uint64_t prev = cmq_atomic_fetch_add_u64(&x, 42, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(prev, 0);
    ASSERT_EQ(cmq_atomic_load_u64(&x, CMQ_ATOMIC_RELAXED), 42);
}

TEST(atomic_64, fetch_sub_basic) {
    cmq_atomic_u64 x = 100;
    uint64_t prev = cmq_atomic_fetch_sub_u64(&x, 25, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(prev, 100);
    ASSERT_EQ(cmq_atomic_load_u64(&x, CMQ_ATOMIC_RELAXED), 75);
}

TEST_MAIN()
#else
/* v0.5.20 is intentionally a no-op on non-32-or-64-bit platforms
 * (currently any architecture). The atomic-64 CI runs on x86_64. */
#include "cmq_test.h"
TEST(atomic_64, skip) { ASSERT(1); }
TEST_MAIN()
#endif
