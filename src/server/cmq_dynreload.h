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
 * Empty/omitted paths leave the slot's current files. nslots 0–4.
 * Non-empty `..` / `\` / controls fail closed before any set. */
int cmq_reload_apply_tls(cmq_tls_config_t **slots, int nslots,
                         const cmq_config_t *fresh);

/* Copy non-empty TLS paths onto the live config. Empty/omitted
 * keeps the current strings. `..` / `\` fail closed. verify 1
 * copies; 0 keeps. Same string is a no-op. */
int cmq_reload_apply_tls_live(cmq_config_t *live, const cmq_config_t *fresh);

/* Copy non-empty acl_allow / acl_deny onto the live config.
 * Empty/omitted keeps the current strings. `..` / `\` fail
 * closed. Same string is a no-op. */
int cmq_reload_apply_acl_live(cmq_config_t *live, const cmq_config_t *fresh);

/* Copy a non-empty mqtt_bridge_maps table onto the live config.
 * Count 0 / omitted keeps the current table. `..` / `\` / empty
 * subject or topic / qos outside 0–2 fail closed. Same table
 * is a no-op. */
int cmq_reload_apply_mqtt_maps_live(cmq_config_t *live,
                                    const cmq_config_t *fresh);

/* Copy non-empty auth / JWT / nkey fields onto the live config.
 * Empty/omitted strings and jwt_leeway_sec 0 keep the current
 * values. jwt_leeway_sec must be 0–3600. All-or-nothing. */
int cmq_reload_apply_auth(cmq_config_t *live, const cmq_config_t *fresh);

/* Copy non-zero live rate / timeout scalars. 0 keeps current.
 * max_connects_per_sec / inbox_max_pending 0–100000;
 * ping / write timeout 0–86400000. All-or-nothing. */
int cmq_reload_apply_limits(cmq_config_t *live, const cmq_config_t *fresh);

/* Copy non-zero payload / sub / client caps. 0 keeps current.
 * max_payload_size 0–CMQ_MAX_PAYLOAD_LIMIT;
 * max_subs_per_client 0–1024; max_clients 0–CMQ_MAX_CLIENTS_LIMIT. */
int cmq_reload_apply_caps(cmq_config_t *live, const cmq_config_t *fresh);

/* Point the next SIGHUP at a new config_file. Empty/omitted
 * keeps the current path. Same string is a no-op. `..` / `\`
 * / controls / len ≥ 512 fail closed (persist_dir rules). */
int cmq_reload_apply_config_file(const char **live, const char *fresh);

/* Async-signal-safe SIGHUP latch. note() from the handler; take()
 * from the event loop. */
void cmq_sighup_note(void);
int cmq_sighup_take(void);

#endif
