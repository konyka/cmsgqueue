#ifndef CMQ_AUDIT_H
#define CMQ_AUDIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F13: Structured audit log.
 *
 * JSON-lines events. Each event is one line. Written to stderr
 * and/or an audit file (when persist_dir is set). The audit file
 * is cmq-audit.log in persist_dir.
 *
 * Events:
 *   auth_ok, auth_fail, persist_fail, persist_recover,
 *   tls_handshake_fail, rate_limit_reject.
 */

typedef enum {
    CMQ_AUDIT_AUTH_OK = 1,
    CMQ_AUDIT_AUTH_FAIL,
    CMQ_AUDIT_PERSIST_FAIL,
    CMQ_AUDIT_PERSIST_RECOVER,
    CMQ_AUDIT_TLS_HANDSHAKE_FAIL,
    CMQ_AUDIT_RATE_LIMIT_REJECT,
} cmq_audit_event_t;

/* Set the audit file path. NULL disables file output. */
void cmq_audit_set_path(const char *path);

const char *cmq_audit_event_name(cmq_audit_event_t event);

/* Write a JSON-lines event. The trace_id is the 16-byte connection
 * ID (hex-encoded) or NULL for non-connection events. */
void cmq_audit_log(cmq_audit_event_t event, const char *trace_id,
                    const char *subject, const char *details);

/* CONNECT helper. `reason` must not contain a password. */
void cmq_audit_auth(int ok, const char *trace_id, const char *user,
                    const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_AUDIT_H */
