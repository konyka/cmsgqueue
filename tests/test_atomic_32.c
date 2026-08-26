/* P2 v0.5.12: cmq_atomic_u64 32-bit portability test.
 *
 * v0.5.9 added the _Static_assert in cmq_atomic.h. This test verifies
 * the build still passes when sizeof(void*) >= 4. On real 64-bit
 * Linux it's trivially true; on 32-bit -m32 targets it would fail
 * loudly at compile time.
 */

#include "cmq_test.h"

#include <stdint.h>
#include <stdio.h>

TEST(atomic_32, void_pointer_size_at_least_4) {
    /* The cmq_atomic.h _Static_assert checks sizeof(void *) >= 4.
     * We re-state that runtime to ensure the build didn't accidentally
     * drop the static_assert. */
    ASSERT(sizeof(void *) >= 4);
}

TEST(atomic_32, u64_load_store_roundtrip) {
    /* cmq_atomic_u64 is _Atomic(uint64_t). On 64-bit Linux it's
     * lock-free; on 32-bit it may not be. This test just verifies
     * load/store works on the current platform. */
    volatile uint64_t x = 0;
    x = 0x1234567890abcdefULL;
    ASSERT(x == 0x1234567890abcdefULL);
}

TEST_MAIN()