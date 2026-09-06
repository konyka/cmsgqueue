#ifndef CMQ_ACCOUNT_H
#define CMQ_ACCOUNT_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_ACCOUNT_MAX         128
#define CMQ_ACCOUNT_NAME_SIZE   64
#define CMQ_ACCOUNT_MAX_EXPORTS 32
#define CMQ_ACCOUNT_MAX_IMPORTS 32

typedef struct cmq_account_manager cmq_account_manager_t;

typedef struct {
    char name[CMQ_ACCOUNT_NAME_SIZE];
    uint64_t connections;
    uint64_t subscriptions;
    uint64_t messages_in;
    uint64_t messages_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    int active;
    uint32_t epoch; /* bumps on soft-delete reactivate; stale clients must not revive */
    uint32_t clear_gen; /* bumps on clear_counters; pairs with stale-inc undo */
    /* v0.5.48: concurrent / per-message hard caps. 0 = unlimited. */
    uint32_t max_connections;
    uint32_t max_subscriptions;
    uint64_t max_payload;
} cmq_account_t;

typedef struct {
    char subject[256];
    char dest_account[CMQ_ACCOUNT_NAME_SIZE];
    int active;
} cmq_account_export_t;

typedef struct {
    char subject[256];
    char source_account[CMQ_ACCOUNT_NAME_SIZE];
    int active;
} cmq_account_import_t;

cmq_account_manager_t *cmq_account_manager_create(void);
void cmq_account_manager_destroy(cmq_account_manager_t *mgr);

int cmq_account_create(cmq_account_manager_t *mgr, const char *name);
/* Like create, but refuses soft-deleted names (CONNECT must not revive).
   Returns 0 if already active or newly created; -1 if soft-deleted / OOM. */
int cmq_account_ensure(cmq_account_manager_t *mgr, const char *name);
int cmq_account_delete(cmq_account_manager_t *mgr, const char *name);
/* out_epoch (optional): snapshot under lock — pass to inc/dec to reject reclaim races. */
cmq_account_t *cmq_account_get(cmq_account_manager_t *mgr, const char *name,
                                uint32_t *out_epoch);
/* Pair with every successful get() before manager destroy. */
void cmq_account_release(cmq_account_manager_t *mgr, cmq_account_t *acc);
size_t cmq_account_count(cmq_account_manager_t *mgr);

void cmq_account_inc_stat(cmq_account_t *acc, size_t field_offset, uint64_t delta);
/* Defaults copy onto newly created / reclaimed slots only. */
void cmq_account_manager_set_defaults(cmq_account_manager_t *mgr,
                                      uint32_t max_conn, uint32_t max_sub,
                                      uint64_t max_payload);
void cmq_account_set_limits(cmq_account_t *acc, uint32_t max_conn,
                            uint32_t max_sub, uint64_t max_payload);
/* 0 if admitted; -1 if acc is NULL or bytes exceed max_payload. */
int cmq_account_check_payload(const cmq_account_t *acc, uint64_t bytes);
/* Returns 0 if the connection credit stuck on this epoch; -1 if
   inactive/raced; -2 if at max_connections. */
int cmq_account_inc_connections(cmq_account_t *acc, uint32_t epoch);
void cmq_account_dec_connections(cmq_account_t *acc, uint32_t epoch);
/* Returns 0 if the subscription credit stuck on this epoch; -1 if
   inactive/raced; -2 if at max_subscriptions. */
int cmq_account_inc_subscriptions(cmq_account_t *acc, uint32_t epoch);
void cmq_account_dec_subscriptions(cmq_account_t *acc, uint32_t epoch);
void cmq_account_inc_msgs_in(cmq_account_t *acc, uint32_t epoch, uint64_t bytes);
void cmq_account_inc_msgs_out(cmq_account_t *acc, uint32_t epoch, uint64_t bytes);

int cmq_account_add_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *dest_account);
int cmq_account_remove_export(cmq_account_manager_t *mgr, const char *account,
                               const char *subject);
size_t cmq_account_export_count(cmq_account_manager_t *mgr, const char *account);

int cmq_account_add_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *source_account);
int cmq_account_remove_import(cmq_account_manager_t *mgr, const char *account,
                               const char *subject);
size_t cmq_account_import_count(cmq_account_manager_t *mgr, const char *account);

int cmq_account_can_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject);
int cmq_account_can_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject);
/* Cross-account delivery gate: same account always ok; when export/import
   lists are non-empty, dest/source account must match (or "*"). */
int cmq_account_may_deliver(cmq_account_manager_t *mgr, const char *pub_account,
                             const char *sub_account, const char *subject);

#endif
