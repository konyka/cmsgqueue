#ifndef CMQ_SUBLIST_PERSIST_H
#define CMQ_SUBLIST_PERSIST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F18: Persistent subscription state.
 *
 * STUB. The full persistent sublist requires refactoring the
 * in-memory cmq_sublist to also write to a WAL stream. The
 * minimal API below is the contract; the implementation is
 * deferred (sublist currently has no WAL integration).
 *
 * The flow is:
 *   1. SUBSCRIBE: cmq_sublist_add writes the sub to the WAL.
 *   2. UNSUBSCRIBE: cmq_sublist_remove deletes from the WAL.
 *   3. Startup: cmq_sublist_persist_load reads all entries from
 *      the WAL into the in-memory sublist.
 *   4. Recovery: replay loop dispatches persisted publishes to
 *      currently-subscribed subjects only.
 *
 * On restart, subscriptions in the WAL are restored. Clients
 * that were connected at the time of the crash are NOT restored;
 * only the server-side state. The recovery loop's handle_publish
 * already iterates records and dispatches via the in-memory sublist.
 */

typedef struct cmq_sublist_persist cmq_sublist_persist_t;

cmq_sublist_persist_t *cmq_sublist_persist_open(const char *dir);
void cmq_sublist_persist_close(cmq_sublist_persist_t *p);

/* Write a SUBSCRIBE. Returns 0 on success, -1 on failure.
 * `subject` and `account` are NUL-terminated. */
int cmq_sublist_persist_record_sub(cmq_sublist_persist_t *p,
                                    uint64_t sub_id, const char *subject,
                                    const char *account);

/* Write an UNSUBSCRIBE. */
int cmq_sublist_persist_record_unsub(cmq_sublist_persist_t *p,
                                      uint64_t sub_id);

/* Read all records and invoke the callback for each. Returns
 * count of records read, or -1 on error. */
typedef int (*cmq_sublist_persist_cb)(void *ctx, int is_sub,
                                       uint64_t sub_id,
                                       const char *subject,
                                       const char *account);
int cmq_sublist_persist_load(cmq_sublist_persist_t *p,
                              cmq_sublist_persist_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_SUBLIST_PERSIST_H */
