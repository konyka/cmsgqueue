/* v0.5.174: INFO JSON string quoting for checksum. */
#include "cmq_info.h"
#include "cmq_test.h"
#include <string.h>

TEST(inf, apply) {
    char out[16];
    ASSERT_EQ(cmq_info_json_str("crc32c", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"crc32c\""), 0);
}

TEST(inf, omitted) {
    char out[8];
    memset(out, 0x5a, sizeof(out));
    ASSERT_EQ(cmq_info_json_str(NULL, out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"\""), 0);
}

TEST(inf, empty) {
    char out[8];
    ASSERT_EQ(cmq_info_json_str("", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "\"\""), 0);
}

TEST(inf, reject) {
    char out[16];
    ASSERT(cmq_info_json_str("crc32c", NULL, 16) != 0);
    ASSERT(cmq_info_json_str("crc32c", out, 2) != 0);
    ASSERT(cmq_info_json_str("a\"b", out, sizeof(out)) != 0);
    ASSERT(cmq_info_json_str("a\\b", out, sizeof(out)) != 0);
    ASSERT(cmq_info_json_str("a\nb", out, sizeof(out)) != 0);
}

TEST_MAIN()
