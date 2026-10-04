/* v0.5.122: reload applies live rate / timeout scalars. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <string.h>

TEST(lim, apply) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_connects_per_sec = 10;
    live.inbox_max_pending = 8;
    live.ping_interval_ms = 30000;
    live.write_timeout_ms = 5000;
    fresh.max_connects_per_sec = 50;
    fresh.inbox_max_pending = 32;
    fresh.ping_interval_ms = 15000;
    fresh.write_timeout_ms = 2000;
    ASSERT_EQ(cmq_reload_apply_limits(&live, &fresh), 0);
    ASSERT_EQ(live.max_connects_per_sec, 50);
    ASSERT_EQ(live.inbox_max_pending, 32);
    ASSERT_EQ(live.ping_interval_ms, 15000);
    ASSERT_EQ(live.write_timeout_ms, 2000);
}

TEST(lim, omitted) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_connects_per_sec = 10;
    live.inbox_max_pending = 8;
    live.ping_interval_ms = 30000;
    live.write_timeout_ms = 5000;
    ASSERT_EQ(cmq_reload_apply_limits(&live, &fresh), 0);
    ASSERT_EQ(live.max_connects_per_sec, 10);
    ASSERT_EQ(live.inbox_max_pending, 8);
    ASSERT_EQ(live.ping_interval_ms, 30000);
    ASSERT_EQ(live.write_timeout_ms, 5000);
}

TEST(lim, empty) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_connects_per_sec = 4;
    live.inbox_max_pending = 2;
    fresh.max_connects_per_sec = 0;
    fresh.inbox_max_pending = 0;
    ASSERT_EQ(cmq_reload_apply_limits(&live, &fresh), 0);
    ASSERT_EQ(live.max_connects_per_sec, 4);
    ASSERT_EQ(live.inbox_max_pending, 2);
}

TEST(lim, reject) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_connects_per_sec = 10;
    live.ping_interval_ms = 30000;
    fresh.max_connects_per_sec = 50;
    fresh.ping_interval_ms = 86400001;
    ASSERT(cmq_reload_apply_limits(&live, &fresh) != 0);
    ASSERT_EQ(live.max_connects_per_sec, 10);
    ASSERT_EQ(live.ping_interval_ms, 30000);
    ASSERT(cmq_reload_apply_limits(NULL, &fresh) != 0);
    ASSERT(cmq_reload_apply_limits(&live, NULL) != 0);
}

TEST_MAIN()
