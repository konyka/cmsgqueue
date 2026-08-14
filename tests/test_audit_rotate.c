/* N3: Audit log rotation test. */

#include "cmq_test.h"
#include "cmq_audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUDIT_TEST_FILE "/tmp/cmq-test-audit-rotate"

TEST(audit_rotate, path_disables_when_null) {
    cmq_audit_set_path(NULL);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, NULL, "u", "d");
}

TEST(audit_rotate, write_creates_file) {
    system("rm -f " AUDIT_TEST_FILE);
    cmq_audit_set_path(AUDIT_TEST_FILE);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, NULL, "u", "d");
    int rc = system("test -f " AUDIT_TEST_FILE);
    ASSERT_EQ(rc, 0);
}

TEST_MAIN()
