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
int cmq_account_delete(cmq_account_manager_t *mgr, const char *name);
cmq_account_t *cmq_account_get(cmq_account_manager_t *mgr, const char *name);
size_t cmq_account_count(cmq_account_manager_t *mgr);

void cmq_account_inc_stat(cmq_account_t *acc, size_t field_offset, uint64_t delta);
void cmq_account_inc_connections(cmq_account_t *acc);
void cmq_account_dec_connections(cmq_account_t *acc);
void cmq_account_inc_subscriptions(cmq_account_t *acc);
void cmq_account_dec_subscriptions(cmq_account_t *acc);
void cmq_account_inc_msgs_in(cmq_account_t *acc, uint64_t bytes);
void cmq_account_inc_msgs_out(cmq_account_t *acc, uint64_t bytes);

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

#endif
