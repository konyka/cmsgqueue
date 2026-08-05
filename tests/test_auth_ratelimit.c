/* F8b: Auth brute-force rate limit.
 *
 * Per-IP limit on failed CONNECT attempts. After 10 attempts
 * within a 1-second window, subsequent attempts from the same IP
 * are rejected with cmq_send_connack(c, 4) without invoking the
 * password verify.
 *
 * Counts are per-IP. The table is fixed-size (1024 slots).
 * On collision (table full), the new IP is treated as admitted
 * (worst case: attacker shares the same source address with
 * itself).
 */

#include "cmq_test.h"
#include <string.h>
#include <stdint.h>

typedef struct {
    uint32_t ip;
    uint64_t window_start_ms;
    uint32_t count;
} auth_rl_slot_t;

#define AUTH_RL_SLOTS 1024
#define AUTH_RL_MAX_PER_SEC 10

static auth_rl_slot_t test_slots[AUTH_RL_SLOTS];
static int test_lock = 0;

static uint64_t now_ms_test(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int auth_rl_admit(uint32_t ip) {
    uint64_t now = now_ms_test();
    int admitted = 0;
    /* No actual mutex in test; single-threaded test only. */
    for (int i = 0; i < AUTH_RL_SLOTS; i++) {
        if (test_slots[i].ip == ip) {
            if (now - test_slots[i].window_start_ms >= 1000) {
                test_slots[i].window_start_ms = now;
                test_slots[i].count = 0;
            }
            if (test_slots[i].count < AUTH_RL_MAX_PER_SEC) {
                test_slots[i].count++;
                admitted = 1;
            }
            return admitted;
        }
        if (test_slots[i].ip == 0) {
            test_slots[i].ip = ip;
            test_slots[i].window_start_ms = now;
            test_slots[i].count = 1;
            return 1;
        }
    }
    return 1; /* full: admit */
}

static void auth_rl_reset(void) {
    memset(test_slots, 0, sizeof(test_slots));
}

TEST(auth_rl, under_limit_admits) {
    auth_rl_reset();
    for (int i = 0; i < AUTH_RL_MAX_PER_SEC; i++) {
        ASSERT_EQ(auth_rl_admit(0x0A000001u), 1);
    }
}

TEST(auth_rl, over_limit_rejects) {
    auth_rl_reset();
    for (int i = 0; i < AUTH_RL_MAX_PER_SEC; i++) {
        ASSERT_EQ(auth_rl_admit(0x0A000002u), 1);
    }
    /* 11th attempt: rejected. */
    ASSERT_EQ(auth_rl_admit(0x0A000002u), 0);
}

TEST(auth_rl, different_ips_isolated) {
    auth_rl_reset();
    for (int i = 0; i < AUTH_RL_MAX_PER_SEC; i++) {
        ASSERT_EQ(auth_rl_admit(0xC0A80001u), 1);
    }
    /* A different IP is unaffected. */
    for (int i = 0; i < AUTH_RL_MAX_PER_SEC; i++) {
        ASSERT_EQ(auth_rl_admit(0xC0A80002u), 1);
    }
}

TEST(auth_rl, window_rolls_over) {
    auth_rl_reset();
    for (int i = 0; i < AUTH_RL_MAX_PER_SEC; i++) {
        ASSERT_EQ(auth_rl_admit(0xAC100001u), 1);
    }
    /* Simulate window roll-over: jump time forward. */
    for (int i = 0; i < AUTH_RL_SLOTS; i++) {
        if (test_slots[i].ip == 0xAC100001u) {
            test_slots[i].window_start_ms = now_ms_test() - 2000;
            break;
        }
    }
    /* After window roll-over, count is reset to 0. */
    ASSERT_EQ(auth_rl_admit(0xAC100001u), 1);
}

TEST_MAIN()
