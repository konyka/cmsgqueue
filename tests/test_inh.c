/* v0.5.175: INFO host comes from live config, not a hardcoded 0.0.0.0. */
#include "cmq_info.h"
#include "cmq_test.h"
#include <string.h>

TEST(inh, apply) {
    char out[32];
    ASSERT_EQ(cmq_info_host_json("127.0.0.1", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"127.0.0.1\""), 0);
}

TEST(inh, omitted) {
    char out[32];
    memset(out, 0x5a, sizeof(out));
    ASSERT_EQ(cmq_info_host_json(NULL, out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"0.0.0.0\""), 0);
}

TEST(inh, empty) {
    char out[32];
    ASSERT_EQ(cmq_info_host_json("", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"0.0.0.0\""), 0);
}

TEST(inh, reject) {
    char out[32];
    ASSERT(cmq_info_host_json("localhost", out, sizeof(out)) != 0);
    ASSERT(cmq_info_host_json("256.0.0.1", out, sizeof(out)) != 0);
    ASSERT(cmq_info_host_json("1.2.3.4\"", out, sizeof(out)) != 0);
    ASSERT(cmq_info_host_json("127.0.0.1", NULL, 32) != 0);
}

TEST_MAIN()
