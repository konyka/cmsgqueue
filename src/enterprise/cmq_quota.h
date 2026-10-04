#ifndef CMQ_QUOTA_H
#define CMQ_QUOTA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F14: Per-account quota enforcement.
 *
 * Token-bucket per account. Configurable caps:
 *   - max_msgs_in_per_sec
 *   - max_bytes_in_per_sec
 *   - max_connections
 *
 * The check is at the credit_msgs_in path (F5). On exceed, the
 * publish is rejected with cmq_send_error("quota exceeded").
 *
 * Per-account state is in cmq_account. The counter is checked
 * on the account's atomic counters.
 */

typedef struct cmq_quota cmq_quota_t;

cmq_quota_t *cmq_quota_create(uint32_t max_msgs_per_sec,
                                uint32_t max_bytes_per_sec,
                                uint32_t max_connections);
void cmq_quota_free(cmq_quota_t *q);
uint32_t cmq_quota_max_msgs(const cmq_quota_t *q);
uint32_t cmq_quota_max_bytes(const cmq_quota_t *q);
uint32_t cmq_quota_max_connects(const cmq_quota_t *q);
/* v0.5.124: in-place cap update. 0 keeps the current field.
 * Creates *q when it is NULL and any cap is non-zero.
 * msgs 0–10000000, bytes 0–1073741824, conns 0–1000000. */
int cmq_quota_reload(cmq_quota_t **q, int msgs, int bytes, int conns);

/* Returns 1 if accepted, 0 if rejected. */
int cmq_quota_check_publish(cmq_quota_t *q, const char *account,
                            size_t msg_len);
int cmq_quota_check_connect(cmq_quota_t *q, const char *account);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_QUOTA_H */
