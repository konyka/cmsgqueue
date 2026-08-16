#ifndef CMQ_SUBJECT_RL_H
#define CMQ_SUBJECT_RL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* N1: per-subject rate limit.
 *
 * Token bucket per subject. Each subject gets its own bucket;
 * capacity is `max_msgs_per_subject` per second. Returns 1 if
 * accepted, 0 if rejected. Use NULL for the singleton (no limit).
 */

typedef struct cmq_subject_rl cmq_subject_rl_t;

cmq_subject_rl_t *cmq_subject_rl_create(uint32_t max_msgs_per_sec);
void cmq_subject_rl_free(cmq_subject_rl_t *rl);
int cmq_subject_rl_check(cmq_subject_rl_t *rl, const char *subject);

#ifdef __cplusplus
}
#endif

#endif
