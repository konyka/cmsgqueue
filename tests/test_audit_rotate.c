/* N3: Audit log rotation test. */

#include "cmq_test.h"
#include "cmq_audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define AUDIT_TEST_FILE   "/tmp/cmq-test-audit-rotate"
#define AUDIT_TEST_ROT    AUDIT_TEST_FILE ".1"

static void clear_audit_artifacts(void) {
    (void)system("rm -f " AUDIT_TEST_FILE " " AUDIT_TEST_ROT);
}

TEST(audit_rotate, path_disables_when_null) {
    cmq_audit_set_path(NULL);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, NULL, "u", "d");
}

TEST(audit_rotate, write_creates_file) {
    clear_audit_artifacts();
    cmq_audit_set_path(AUDIT_TEST_FILE);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, NULL, "u", "d");
    int rc = system("test -f " AUDIT_TEST_FILE);
    ASSERT_EQ(rc, 0);
    clear_audit_artifacts();
}

/* RED: drive rotation past the configured cap by emitting events until
 * the on-disk file exceeds the threshold. Each event produces a ~100
 * byte JSON line so we accumulate. Verify <path> is renamed to
 * <path>.1 and a follow-up event re-creates <path>. Pins the
 * rotation contract from docs/reviews/v0.5.1.plan.md. */
TEST(audit_rotate, rotation_after_cap) {
    clear_audit_artifacts();
    cmq_audit_set_path(AUDIT_TEST_FILE);

    /* Use a small test-only cap so the loop finishes quickly. The
     * cmq_audit_set_max_bytes(0) call below restores the 100 MiB
     * default for any subsequent tests in the binary. */
    cmq_audit_set_max_bytes(4096);
    int filled = 0;
    for (int i = 0; i < 100000; i++) {
        cmq_audit_log(CMQ_AUDIT_AUTH_OK, "rotate-fill", "u", "d");
        struct stat rot_st;
        if (stat(AUDIT_TEST_ROT, &rot_st) == 0) {
            filled = 1;
            break;
        }
    }
    cmq_audit_set_max_bytes(0);
    ASSERT(filled);

    int rotated = system("test -f " AUDIT_TEST_ROT);
    ASSERT_EQ(rotated, 0);

    cmq_audit_log(CMQ_AUDIT_AUTH_OK, "rotate-after", "u2", "d2");
    struct stat st;
    ASSERT_EQ(stat(AUDIT_TEST_FILE, &st), 0);
    ASSERT(st.st_size > 0);

    clear_audit_artifacts();
}

TEST_MAIN()
