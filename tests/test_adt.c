/* v0.5.129: audit file follows persist_dir. */
#include "cmq_audit.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

TEST(adt, apply) {
    const char *dir = "/tmp/cmq_adt_apply";
    const char *path = "/tmp/cmq_adt_apply/cmq-audit.log";
    (void)mkdir(dir, 0755);
    unlink(path);
    ASSERT_EQ(cmq_audit_from_persist(dir), 0);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, "tid", "alice", "adt-apply");
    ASSERT(file_contains(path, "adt-apply"));
    unlink(path);
    cmq_audit_set_path(NULL);
}

TEST(adt, omitted) {
    const char *dir = "/tmp/cmq_adt_omit";
    const char *path = "/tmp/cmq_adt_omit/cmq-audit.log";
    (void)mkdir(dir, 0755);
    unlink(path);
    ASSERT_EQ(cmq_audit_from_persist(dir), 0);
    ASSERT_EQ(cmq_audit_reload_persist(NULL), 0);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, "tid", "bob", "adt-omit");
    ASSERT(file_contains(path, "adt-omit"));
    unlink(path);
    cmq_audit_set_path(NULL);
}

TEST(adt, empty) {
    const char *dir = "/tmp/cmq_adt_empty";
    const char *path = "/tmp/cmq_adt_empty/cmq-audit.log";
    (void)mkdir(dir, 0755);
    unlink(path);
    ASSERT_EQ(cmq_audit_from_persist(dir), 0);
    ASSERT_EQ(cmq_audit_reload_persist(""), 0);
    cmq_audit_log(CMQ_AUDIT_AUTH_OK, "tid", "cara", "adt-empty");
    ASSERT(file_contains(path, "adt-empty"));
    unlink(path);
    cmq_audit_set_path(NULL);
}

TEST(adt, reject) {
    const char *dir = "/tmp/cmq_adt_rej";
    const char *path = "/tmp/cmq_adt_rej/cmq-audit.log";
    (void)mkdir(dir, 0755);
    unlink(path);
    ASSERT_EQ(cmq_audit_from_persist(dir), 0);
    ASSERT(cmq_audit_from_persist("../evil") != 0);
    ASSERT(cmq_audit_reload_persist("bad\\dir") != 0);
    cmq_audit_log(CMQ_AUDIT_AUTH_FAIL, "tid", "dan", "adt-reject");
    ASSERT(file_contains(path, "adt-reject"));
    unlink(path);
    cmq_audit_set_path(NULL);
}

TEST_MAIN()
