#ifndef CMQ_DYNRELOAD_H
#define CMQ_DYNRELOAD_H

#include "cmq.h"
#include "cmq_log.h"
#include "cmq_rch.h"
#include "cmq_tls.h"

/* Apply dynamic fields from a freshly loaded config.
 * log_level 0–5 is stored and pushed to log (if non-NULL).
 * acl_allow / acl_deny rebuild the ACL handle when either is set. */
int cmq_reload_apply_dynamic(cmq_log_t *log, int *log_level,
                             cmq_rch_t **acl_h,
                             const cmq_config_t *fresh);

/* Push fresh TLS paths onto live slots and cmq_tls_reload each.
 * Empty/omitted paths leave the slot's current files. nslots 0–4. */
int cmq_reload_apply_tls(cmq_tls_config_t **slots, int nslots,
                         const cmq_config_t *fresh);

#endif
