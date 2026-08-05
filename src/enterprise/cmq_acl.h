#ifndef CMQ_ACL_H
#define CMQ_ACL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F16: Per-account subject allow-list / deny-list.
 *
 * The matcher supports NATS-style wildcards:
 *   foo.*     matches "foo.bar" but not "foo.bar.baz"
 *   foo.>     matches "foo.bar.baz" and any deeper
 *   foo       exact match only
 *
 * Default: no lists (always admit). With only a deny-list, admit
 * unless the subject matches. With only an allow-list, admit only
 * if the subject matches. With both, deny-list wins (defense in
 * depth).
 *
 * Used by handle_publish before cmq_sublist_match. Reject with
 * cmq_send_error("permission denied").
 */

typedef struct cmq_acl cmq_acl_t;

cmq_acl_t *cmq_acl_create(void);
void cmq_acl_free(cmq_acl_t *acl);

/* Add a pattern to the allow-list. */
int cmq_acl_allow(cmq_acl_t *acl, const char *pattern);

/* Add a pattern to the deny-list. */
int cmq_acl_deny(cmq_acl_t *acl, const char *pattern);

/* Returns 1 if the subject is admitted, 0 if denied. */
int cmq_acl_check(cmq_acl_t *acl, const char *subject);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_ACL_H */
