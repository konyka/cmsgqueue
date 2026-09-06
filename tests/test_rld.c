/* v0.5.116: reload applies log_level and acl_deny. */
#include "cmq_dynreload.h"
#include "cmq_acl.h"
#include "cmq_test.h"
#include <string.h>

static int acl_admits(cmq_rch_t *h, const char *subject) {
    cmq_acl_t *acl = (cmq_acl_t *)cmq_rch_acquire(h);
    int ok = acl ? cmq_acl_check(acl, subject) : 1;
    cmq_rch_release(h, acl);
    return ok;
}

TEST(rld, apply_log) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    int stored = 2;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.log_level = 4;
    ASSERT_EQ(cmq_reload_apply_dynamic(log, &stored, NULL, &fresh), 0);
    ASSERT_EQ(stored, 4);
    ASSERT_EQ((int)cmq_log_get_level(log), 4);
    cmq_log_destroy(log);
}

TEST(rld, apply_acl_deny) {
    int stored = 2;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.log_level = 2;
    fresh.acl_deny = "secret.>";
    cmq_rch_t *h = NULL;
    ASSERT_EQ(cmq_reload_apply_dynamic(NULL, &stored, &h, &fresh), 0);
    ASSERT(h != NULL);
    ASSERT_EQ(acl_admits(h, "secret.foo"), 0);
    ASSERT_EQ(acl_admits(h, "public.bar"), 1);
    cmq_rch_release_owner(h);
}

TEST(rld, apply_acl_both) {
    int stored = 2;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.log_level = 2;
    fresh.acl_allow = "foo.>";
    fresh.acl_deny = "foo.secret.>";
    cmq_rch_t *h = NULL;
    ASSERT_EQ(cmq_reload_apply_dynamic(NULL, &stored, &h, &fresh), 0);
    ASSERT_EQ(acl_admits(h, "foo.bar"), 1);
    ASSERT_EQ(acl_admits(h, "foo.secret.x"), 0);
    ASSERT_EQ(acl_admits(h, "other"), 0);
    cmq_rch_release_owner(h);
}

TEST(rld, reject) {
    int stored = 2;
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.log_level = 6;
    ASSERT(cmq_reload_apply_dynamic(NULL, &stored, NULL, &fresh) != 0);
    ASSERT_EQ(stored, 2);
}

TEST_MAIN()
