/* v0.5.176: INFO server_id comes from live cluster_node_id. */
#include "cmq_info.h"
#include "cmq_test.h"
#include <string.h>

TEST(isd, apply) {
    char out[32];
    ASSERT_EQ(cmq_info_server_id_json("n1", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"n1\""), 0);
}

TEST(isd, omitted) {
    char out[32];
    memset(out, 0x5a, sizeof(out));
    ASSERT_EQ(cmq_info_server_id_json(NULL, out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"cmsgsrv\""), 0);
}

TEST(isd, empty) {
    char out[32];
    ASSERT_EQ(cmq_info_server_id_json("", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"cmsgsrv\""), 0);
}

TEST(isd, reject) {
    char out[32];
    ASSERT(cmq_info_server_id_json("a\"b", out, sizeof(out)) != 0);
    ASSERT(cmq_info_server_id_json("0123456789abcdef", out, sizeof(out)) != 0);
    ASSERT(cmq_info_server_id_json("n1", NULL, 32) != 0);
}

TEST_MAIN()
