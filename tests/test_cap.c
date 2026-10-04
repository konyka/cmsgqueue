/* v0.5.123: reload applies payload / sub / client caps. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <string.h>

TEST(cap, apply) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_payload_size = 1024;
    live.max_subs_per_client = 16;
    live.max_clients = 64;
    fresh.max_payload_size = 4096;
    fresh.max_subs_per_client = 32;
    fresh.max_clients = 128;
    ASSERT_EQ(cmq_reload_apply_caps(&live, &fresh), 0);
    ASSERT_EQ(live.max_payload_size, 4096);
    ASSERT_EQ(live.max_subs_per_client, 32);
    ASSERT_EQ(live.max_clients, 128);
}

TEST(cap, omitted) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_payload_size = 1024;
    live.max_subs_per_client = 16;
    live.max_clients = 64;
    ASSERT_EQ(cmq_reload_apply_caps(&live, &fresh), 0);
    ASSERT_EQ(live.max_payload_size, 1024);
    ASSERT_EQ(live.max_subs_per_client, 16);
    ASSERT_EQ(live.max_clients, 64);
}

TEST(cap, empty) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_payload_size = 2048;
    fresh.max_payload_size = 0;
    fresh.max_subs_per_client = 0;
    fresh.max_clients = 0;
    ASSERT_EQ(cmq_reload_apply_caps(&live, &fresh), 0);
    ASSERT_EQ(live.max_payload_size, 2048);
}

TEST(cap, reject) {
    cmq_config_t live, fresh;
    memset(&live, 0, sizeof(live));
    memset(&fresh, 0, sizeof(fresh));
    live.max_payload_size = 1024;
    fresh.max_payload_size = CMQ_MAX_PAYLOAD_LIMIT + 1;
    ASSERT(cmq_reload_apply_caps(&live, &fresh) != 0);
    ASSERT_EQ(live.max_payload_size, 1024);
    fresh.max_payload_size = 0;
    fresh.max_subs_per_client = CMQ_DEFAULT_MAX_SUBS_PER_CLIENT + 1;
    ASSERT(cmq_reload_apply_caps(&live, &fresh) != 0);
    ASSERT(cmq_reload_apply_caps(NULL, &fresh) != 0);
    ASSERT(cmq_reload_apply_caps(&live, NULL) != 0);
}

TEST_MAIN()
