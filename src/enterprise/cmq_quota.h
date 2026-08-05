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

/* Returns 1 if accepted, 0 if rejected. */
int cmq_quota_check_publish(cmq_quota_t *q, const char *account,
                            size_t msg_len);
int cmq_quota_check_connect(cmq_quota_t *q, const char *account);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_QUOTA_H */
