#define _POSIX_C_SOURCE 200809L
#include "cmq_audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

static char *g_audit_path = NULL;
static pthread_mutex_t g_audit_lock = PTHREAD_MUTEX_INITIALIZER;

const char *cmq_audit_event_name(cmq_audit_event_t e) {
    switch (e) {
        case CMQ_AUDIT_AUTH_OK: return "auth_ok";
        case CMQ_AUDIT_AUTH_FAIL: return "auth_fail";
        case CMQ_AUDIT_PERSIST_FAIL: return "persist_fail";
        case CMQ_AUDIT_PERSIST_RECOVER: return "persist_recover";
        case CMQ_AUDIT_TLS_HANDSHAKE_FAIL: return "tls_handshake_fail";
        case CMQ_AUDIT_RATE_LIMIT_REJECT: return "rate_limit_reject";
        default: return "unknown";
    }
}

#define AUDIT_MAX_BYTES (100u * 1024u * 1024u)

void cmq_audit_set_path(const char *path) {
    pthread_mutex_lock(&g_audit_lock);
    free(g_audit_path);
    g_audit_path = path ? strdup(path) : NULL;
    pthread_mutex_unlock(&g_audit_lock);
}

static void json_escape(const char *in, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 > out_cap) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\n' || c == '\r') {
            if (o + 3 > out_cap) break;
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c < 0x20) {
            if (o + 7 > out_cap) break;
            o += (size_t)snprintf(out + o, out_cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

void cmq_audit_log(cmq_audit_event_t event, const char *trace_id,
                    const char *subject, const char *details) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char ts_buf[64];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm);

    char subj_esc[256];
    char det_esc[512];
    json_escape(subject ? subject : "", subj_esc, sizeof(subj_esc));
    json_escape(details ? details : "", det_esc, sizeof(det_esc));

    char line[1024];
    int n = snprintf(line, sizeof(line),
                     "{\"ts\":\"%s.%03ldZ\",\"event\":\"%s\",\"trace\":\"%s\","
                     "\"subject\":\"%s\",\"details\":\"%s\"}\n",
                     ts_buf, (long)(ts.tv_nsec / 1000000),
                     cmq_audit_event_name(event),
                     trace_id ? trace_id : "",
                     subj_esc, det_esc);
    if (n <= 0 || (size_t)n >= sizeof(line)) return;

    pthread_mutex_lock(&g_audit_lock);
    fputs(line, stderr);
    if (g_audit_path) {
        FILE *f = fopen(g_audit_path, "a");
        if (f) {
            fputs(line, f);
            fflush(f);
            /* P6: rotate when on-disk size exceeds the cap. Use
             * fstat — ftell on append-mode stdio doesn't reliably
             * return the file size (was the source of v0.5.0's
             * silent-non-rotation bug, see v0.5.1.bundle.md B7). */
            struct stat st;
            int needs_rotate = (fstat(fileno(f), &st) == 0 &&
                                 (uint64_t)st.st_size >= AUDIT_MAX_BYTES);
            fclose(f);
            if (needs_rotate) {
                char rotated[1024];
                snprintf(rotated, sizeof(rotated), "%s.1", g_audit_path);
                rename(g_audit_path, rotated);
            }
        }
    }
    pthread_mutex_unlock(&g_audit_lock);
}

void cmq_audit_auth(int ok, const char *trace_id, const char *user,
                    const char *reason) {
    cmq_audit_log(ok ? CMQ_AUDIT_AUTH_OK : CMQ_AUDIT_AUTH_FAIL,
                  trace_id, user, reason);
}
