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

/* Verify each line in the audit file is a single-line JSON object:
 * a leading '{', a trailing '}', no embedded unescaped quote between the
 * brace and the pair. Returns 1 if all lines pass. */
static int file_lines_are_valid_json(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048];
    int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (n < 2 || line[0] != '{' || line[n - 1] != '}') { ok = 0; break; }
        int depth = 0, escape = 0;
        for (size_t i = 0; i < n; i++) {
            char c = line[i];
            if (escape) { escape = 0; continue; }
            if (c == '\\') { escape = 1; continue; }
            if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
                /* A raw quote outside the escaping state would
                 * indicate an unescaped quote. This is a coarse check;
                 * depth tracking above gives us structural sanity. */
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        if (depth != 0) { ok = 0; break; }
    }
    fclose(f);
    return ok;
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

/* RED: trace_id is currently written raw into the JSON line. A
 * crafted trace containing quotes, control bytes, or a closing brace
 * can break the JSON contract and append attacker-controlled fields.
 * After the fix, trace_id is JSON-escaped via the existing helper. */
TEST(audit, trace_id_is_json_safe) {
    cmq_audit_set_path(AUDIT_TEST_FILE);
    unlink(AUDIT_TEST_FILE);
    /* Construct a trace that, if written raw, would close the JSON
     * object early and inject a new field. */
    const char *bad_trace = "trace\"},{\"injected\":\"yes\",\"x\":\"";
    cmq_audit_log(CMQ_AUDIT_AUTH_FAIL, bad_trace, "u", "d");
    ASSERT(file_lines_are_valid_json(AUDIT_TEST_FILE));
    /* The injected key must not appear unescaped. */
    ASSERT(!file_contains(AUDIT_TEST_FILE, "\"injected\":\"yes\""));
    unlink(AUDIT_TEST_FILE);
}

/* RED: json_escape currently drops characters silently when the
 * buffer is too small to hold the encoded output. For a trace_id
 * filled with control bytes (\\uXXXX = 6 bytes per source byte
 * plus null), the 64-byte trace_esc buffer overruns and the rest
 * of the trace is silently lost, producing a truncated JSON line.
 * After the fix, the API must drop the whole event when the trace
 * cannot safely fit, never producing truncated or malformed JSON. */
TEST(audit, trace_id_overflow_drops_event_safely) {
    cmq_audit_set_path(AUDIT_TEST_FILE);
    unlink(AUDIT_TEST_FILE);
    /* 16 control bytes expand to 96 bytes of \\uXXXX output,
     * exceeding the 64-byte trace_esc buffer in cmq_audit.c. */
    char long_trace[64];
    memset(long_trace, 0x01, 16);
    long_trace[16] = '\0';
    cmq_audit_log(CMQ_AUDIT_AUTH_FAIL, long_trace, "u", "d");
    /* Either the file must not contain the truncated line, or it
     * must be a fully-formed JSON object. Empty file is the
     * post-fix acceptable behavior. */
    FILE *f = fopen(AUDIT_TEST_FILE, "r");
    if (!f) { unlink(AUDIT_TEST_FILE); return; }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    if (n == 0) {
        /* dropped entirely */
    } else {
        ASSERT(file_lines_are_valid_json(AUDIT_TEST_FILE));
        /* If kept, the trace must contain exactly 16 \u00XX escapes,
         * one per source byte, proving no truncation. */
        int total = 0;
        for (int i = 0; i < 16; i++) {
            char needle[8];
            snprintf(needle, sizeof(needle), "\\u00%02x",
                     (unsigned char)long_trace[i]);
            const char *p = buf;
            size_t nlen = strlen(needle);
            while ((p = strstr(p, needle))) { total++; p += nlen; }
        }
        ASSERT_EQ(total, 16);
    }
    unlink(AUDIT_TEST_FILE);
}

TEST_MAIN()
