#ifndef CMQ_BLOCKLIST_H
#define CMQ_BLOCKLIST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F15: Connection blocklist.
 *
 * Loaded from a file at startup. Format: one IP or CIDR per line.
 * Examples:
 *   10.0.0.1
 *   192.168.0.0/16
 * Malformed lines are skipped.
 *
 * Used by accept_cb to reject banned IPs pre-handshake. Updates
 * via cmq_blocklist_reload are hot (lock-protected).
 */

typedef struct cmq_blocklist cmq_blocklist_t;

cmq_blocklist_t *cmq_blocklist_load(const char *path);
void cmq_blocklist_free(cmq_blocklist_t *bl);
int cmq_blocklist_reload(cmq_blocklist_t *bl, const char *path);

/* Returns 1 if IP is blocked, 0 if admitted. */
int cmq_blocklist_check(const cmq_blocklist_t *bl, uint32_t ip_be);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_BLOCKLIST_H */
