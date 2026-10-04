/* v0.5.152: reload applies config_file for the next SIGHUP. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(cfp, apply) {
    char *live = strdup("/old.conf");
    ASSERT_EQ(cmq_reload_apply_config_file((const char **)&live,
                                           "/new.conf"), 0);
    ASSERT_STR_EQ(live, "/new.conf");
    ASSERT_EQ(cmq_reload_apply_config_file((const char **)&live,
                                           "/new.conf"), 0);
    ASSERT_STR_EQ(live, "/new.conf");
    free(live);
}

TEST(cfp, omitted) {
    char *live = strdup("/keep.conf");
    ASSERT_EQ(cmq_reload_apply_config_file((const char **)&live, NULL), 0);
    ASSERT_STR_EQ(live, "/keep.conf");
    free(live);
}

TEST(cfp, empty) {
    char *live = strdup("/keep.conf");
    ASSERT_EQ(cmq_reload_apply_config_file((const char **)&live, ""), 0);
    ASSERT_STR_EQ(live, "/keep.conf");
    free(live);
}

TEST(cfp, reject) {
    char *live = strdup("/keep.conf");
    ASSERT(cmq_reload_apply_config_file((const char **)&live,
                                        "../evil.conf") != 0);
    ASSERT_STR_EQ(live, "/keep.conf");
    ASSERT(cmq_reload_apply_config_file(NULL, "/ok.conf") != 0);
    ASSERT(cmq_reload_apply_config_file((const char **)&live,
                                        "bad\\path.conf") != 0);
    ASSERT_STR_EQ(live, "/keep.conf");
    free(live);
}

TEST_MAIN()
