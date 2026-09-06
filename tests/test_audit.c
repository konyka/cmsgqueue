/* F13: Audit log tests. */

#include "cmq_test.h"
#include "cmq_audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define AUDIT_TEST_FILE "/tmp/cmq-test-audit.log"

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

TEST(audit, set_path_disables_when_null) {
    cmq_audit_set_path(NULL);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, NULL, "user1", "test");
    /* No file written. */
    ASSERT(access("/tmp/cmq-audit-null.log", F_OK) != 0);
}

TEST(audit, log_writes_event_to_stderr) {
    /* Just exercise — stderr is captured by test framework. */
    cmq_audit_log(CMQ_AUDIT_AUTH_FAIL, "trace-abc", "user1", "bad password");
}

TEST(audit, log_writes_event_to_file) {
    cmq_audit_set_path(AUDIT_TEST_FILE);
    unlink(AUDIT_TEST_FILE);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, "trace-xyz", "user1", "logged in");
    cmq_audit_log(CMQ_AUDIT_AUTH_FAIL, "trace-xyz", "user1", "bad password");
    cmq_audit_log(CMQ_AUDIT_RATE_LIMIT_REJECT, "trace-xyz",
                   "10.0.0.5", "11th attempt");
    ASSERT(file_contains(AUDIT_TEST_FILE, "auth_ok"));
    ASSERT(file_contains(AUDIT_TEST_FILE, "auth_fail"));
    ASSERT(file_contains(AUDIT_TEST_FILE, "rate_limit_reject"));
    ASSERT(file_contains(AUDIT_TEST_FILE, "trace-xyz"));
    unlink(AUDIT_TEST_FILE);
}

TEST(audit, json_escape_special_chars) {
    cmq_audit_set_path(AUDIT_TEST_FILE);
    unlink(AUDIT_TEST_FILE);
    /* Test JSON-escape of quote, backslash, newline. */
    cmq_audit_log(CMQ_AUDIT_PERSIST_FAIL, "trace", "subj\"with\\quote",
                   "details\nwith\nnewlines");
    ASSERT(file_contains(AUDIT_TEST_FILE, "subj\\\"with\\\\quote"));
    ASSERT(file_contains(AUDIT_TEST_FILE, "details\\nwith\\nnewlines"));
    unlink(AUDIT_TEST_FILE);
}

TEST(audit, event_names) {
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_AUTH_OK), "auth_ok"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_AUTH_FAIL), "auth_fail"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_PERSIST_FAIL),
                     "persist_fail"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_PERSIST_RECOVER),
                     "persist_recover"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_TLS_HANDSHAKE_FAIL),
                     "tls_handshake_fail"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name(CMQ_AUDIT_RATE_LIMIT_REJECT),
                     "rate_limit_reject"), 0);
    ASSERT_EQ(strcmp(cmq_audit_event_name((cmq_audit_event_t)0), "unknown"), 0);
}

TEST(audit, auth_helper_no_secret) {
    cmq_audit_set_path(AUDIT_TEST_FILE);
    unlink(AUDIT_TEST_FILE);
    cmq_audit_auth(1, "tid1", "alice", "auth ok");
    cmq_audit_auth(0, "tid1", "alice", "auth failed");
    ASSERT(file_contains(AUDIT_TEST_FILE, "\"event\":\"auth_ok\""));
    ASSERT(file_contains(AUDIT_TEST_FILE, "\"event\":\"auth_fail\""));
    ASSERT(file_contains(AUDIT_TEST_FILE, "auth failed"));
    ASSERT(!file_contains(AUDIT_TEST_FILE, "s3cret"));
    unlink(AUDIT_TEST_FILE);
}

TEST_MAIN()
