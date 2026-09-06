/* v0.5.155: reload copies acl_allow / acl_deny onto live config. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(ala, apply) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.acl_allow = strdup("old.>");
    live.acl_deny = strdup("old.secret.>");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.acl_allow = "foo.>";
    fresh.acl_deny = "foo.secret.>";
    ASSERT_EQ(cmq_reload_apply_acl_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.acl_allow, "foo.>");
    ASSERT_STR_EQ(live.acl_deny, "foo.secret.>");
    ASSERT_EQ(cmq_reload_apply_acl_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.acl_allow, "foo.>");
    free((void *)live.acl_allow);
    free((void *)live.acl_deny);
}

TEST(ala, omitted) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.acl_allow = strdup("keep.>");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_acl_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.acl_allow, "keep.>");
    free((void *)live.acl_allow);
}

TEST(ala, empty) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.acl_deny = strdup("keep.>");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.acl_deny = "";
    ASSERT_EQ(cmq_reload_apply_acl_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.acl_deny, "keep.>");
    free((void *)live.acl_deny);
}

TEST(ala, reject) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.acl_allow = strdup("keep.>");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.acl_allow = "../evil.>";
    ASSERT(cmq_reload_apply_acl_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.acl_allow, "keep.>");
    ASSERT(cmq_reload_apply_acl_live(NULL, &fresh) != 0);
    fresh.acl_allow = "foo.>";
    fresh.acl_deny = "bad\\deny.>";
    ASSERT(cmq_reload_apply_acl_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.acl_allow, "keep.>");
    free((void *)live.acl_allow);
}

TEST_MAIN()
