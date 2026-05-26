#define _POSIX_C_SOURCE 200809L
#include "cmq_account.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>

struct cmq_account_manager {
    cmq_account_t accounts[CMQ_ACCOUNT_MAX];
    size_t count;
    cmq_mutex_t lock;
};

cmq_account_manager_t *cmq_account_manager_create(void) {
    cmq_account_manager_t *mgr = calloc(1, sizeof(cmq_account_manager_t));
    if (!mgr) return NULL;
    cmq_mutex_init(&mgr->lock);
    return mgr;
}

void cmq_account_manager_destroy(cmq_account_manager_t *mgr) {
    if (!mgr) return;
    cmq_mutex_destroy(&mgr->lock);
    free(mgr);
}

int cmq_account_create(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    if (mgr->count >= CMQ_ACCOUNT_MAX) {
        cmq_mutex_unlock(&mgr->lock);
        return -1;
    }
    cmq_account_t *a = &mgr->accounts[mgr->count++];
    strncpy(a->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
    a->active = 1;
    a->connections = 0;
    a->subscriptions = 0;
    a->messages_in = 0;
    a->messages_out = 0;
    a->bytes_in = 0;
    a->bytes_out = 0;
    cmq_mutex_unlock(&mgr->lock);
    return 0;
}

int cmq_account_delete(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            memmove(&mgr->accounts[i], &mgr->accounts[i + 1],
                    (mgr->count - i - 1) * sizeof(cmq_account_t));
            mgr->count--;
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&mgr->lock);
    return -1;
}

cmq_account_t *cmq_account_get(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return NULL;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_t *found = NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            found = &mgr->accounts[i];
            break;
        }
    }
    cmq_mutex_unlock(&mgr->lock);
    return found;
}

size_t cmq_account_count(cmq_account_manager_t *mgr) {
    if (!mgr) return 0;
    cmq_mutex_lock(&mgr->lock);
    size_t c = mgr->count;
    cmq_mutex_unlock(&mgr->lock);
    return c;
}

void cmq_account_inc_connections(cmq_account_t *acc) {
    if (acc) acc->connections++;
}
void cmq_account_dec_connections(cmq_account_t *acc) {
    if (acc && acc->connections > 0) acc->connections--;
}
void cmq_account_inc_subscriptions(cmq_account_t *acc) {
    if (acc) acc->subscriptions++;
}
void cmq_account_dec_subscriptions(cmq_account_t *acc) {
    if (acc && acc->subscriptions > 0) acc->subscriptions--;
}
void cmq_account_inc_msgs_in(cmq_account_t *acc, uint64_t bytes) {
    if (acc) { acc->messages_in++; acc->bytes_in += bytes; }
}
void cmq_account_inc_msgs_out(cmq_account_t *acc, uint64_t bytes) {
    if (acc) { acc->messages_out++; acc->bytes_out += bytes; }
}

typedef struct {
    char account[CMQ_ACCOUNT_NAME_SIZE];
    cmq_account_export_t exports[CMQ_ACCOUNT_MAX_EXPORTS];
    size_t export_count;
    cmq_account_import_t imports[CMQ_ACCOUNT_MAX_IMPORTS];
    size_t import_count;
} cmq_account_perms_t;

static cmq_account_perms_t g_perms[CMQ_ACCOUNT_MAX];
static size_t g_perms_count = 0;
static cmq_mutex_t g_perms_lock;
static int g_perms_init = 0;

static void ensure_perms_init(void) {
    if (!g_perms_init) {
        cmq_mutex_init(&g_perms_lock);
        g_perms_init = 1;
    }
}

static cmq_account_perms_t *find_perms(const char *account) {
    for (size_t i = 0; i < g_perms_count; i++) {
        if (strcmp(g_perms[i].account, account) == 0) return &g_perms[i];
    }
    return NULL;
}

static cmq_account_perms_t *find_or_create_perms(const char *account) {
    cmq_account_perms_t *p = find_perms(account);
    if (p) return p;
    if (g_perms_count >= CMQ_ACCOUNT_MAX) return NULL;
    p = &g_perms[g_perms_count++];
    strncpy(p->account, account, CMQ_ACCOUNT_NAME_SIZE - 1);
    return p;
}

int cmq_account_add_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *dest_account) {
    (void)mgr;
    if (!account || !subject || !dest_account) return -1;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_or_create_perms(account);
    if (!p) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    if (p->export_count >= CMQ_ACCOUNT_MAX_EXPORTS) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    for (size_t i = 0; i < p->export_count; i++) {
        if (strcmp(p->exports[i].subject, subject) == 0 &&
            strcmp(p->exports[i].dest_account, dest_account) == 0) {
            cmq_mutex_unlock(&g_perms_lock);
            return 0;
        }
    }
    cmq_account_export_t *e = &p->exports[p->export_count++];
    strncpy(e->subject, subject, sizeof(e->subject) - 1);
    strncpy(e->dest_account, dest_account, CMQ_ACCOUNT_NAME_SIZE - 1);
    e->active = 1;
    cmq_mutex_unlock(&g_perms_lock);
    return 0;
}

int cmq_account_remove_export(cmq_account_manager_t *mgr, const char *account,
                               const char *subject) {
    (void)mgr;
    if (!account || !subject) return -1;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    if (!p) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    for (size_t i = 0; i < p->export_count; i++) {
        if (strcmp(p->exports[i].subject, subject) == 0) {
            memmove(&p->exports[i], &p->exports[i + 1],
                    (p->export_count - i - 1) * sizeof(cmq_account_export_t));
            p->export_count--;
            cmq_mutex_unlock(&g_perms_lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&g_perms_lock);
    return -1;
}

size_t cmq_account_export_count(cmq_account_manager_t *mgr, const char *account) {
    (void)mgr;
    if (!account) return 0;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    size_t c = p ? p->export_count : 0;
    cmq_mutex_unlock(&g_perms_lock);
    return c;
}

int cmq_account_add_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *source_account) {
    (void)mgr;
    if (!account || !subject || !source_account) return -1;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_or_create_perms(account);
    if (!p) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    if (p->import_count >= CMQ_ACCOUNT_MAX_IMPORTS) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    for (size_t i = 0; i < p->import_count; i++) {
        if (strcmp(p->imports[i].subject, subject) == 0 &&
            strcmp(p->imports[i].source_account, source_account) == 0) {
            cmq_mutex_unlock(&g_perms_lock);
            return 0;
        }
    }
    cmq_account_import_t *imp = &p->imports[p->import_count++];
    strncpy(imp->subject, subject, sizeof(imp->subject) - 1);
    strncpy(imp->source_account, source_account, CMQ_ACCOUNT_NAME_SIZE - 1);
    imp->active = 1;
    cmq_mutex_unlock(&g_perms_lock);
    return 0;
}

int cmq_account_remove_import(cmq_account_manager_t *mgr, const char *account,
                               const char *subject) {
    (void)mgr;
    if (!account || !subject) return -1;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    if (!p) { cmq_mutex_unlock(&g_perms_lock); return -1; }
    for (size_t i = 0; i < p->import_count; i++) {
        if (strcmp(p->imports[i].subject, subject) == 0) {
            memmove(&p->imports[i], &p->imports[i + 1],
                    (p->import_count - i - 1) * sizeof(cmq_account_import_t));
            p->import_count--;
            cmq_mutex_unlock(&g_perms_lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&g_perms_lock);
    return -1;
}

size_t cmq_account_import_count(cmq_account_manager_t *mgr, const char *account) {
    (void)mgr;
    if (!account) return 0;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    size_t c = p ? p->import_count : 0;
    cmq_mutex_unlock(&g_perms_lock);
    return c;
}

static int subject_match(const char *pattern, const char *subject) {
    if (strcmp(pattern, ">") == 0) return 1;
    if (strcmp(pattern, subject) == 0) return 1;
    size_t plen = strlen(pattern);
    if (plen > 1 && pattern[plen - 1] == '>') {
        size_t prefix_len = plen - 1;
        if (prefix_len > 0 && pattern[prefix_len - 1] == '.')
            prefix_len--;
        return strncmp(pattern, subject, prefix_len) == 0 &&
               (subject[prefix_len] == '.' || subject[prefix_len] == '\0');
    }
    return 0;
}

int cmq_account_can_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject) {
    (void)mgr;
    if (!account || !subject) return 0;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    int ok = 0;
    if (p) {
        for (size_t i = 0; i < p->import_count; i++) {
            if (subject_match(p->imports[i].subject, subject)) {
                ok = 1;
                break;
            }
        }
    }
    cmq_mutex_unlock(&g_perms_lock);
    return ok;
}

int cmq_account_can_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject) {
    (void)mgr;
    if (!account || !subject) return 0;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    int ok = 0;
    if (p) {
        for (size_t i = 0; i < p->export_count; i++) {
            if (subject_match(p->exports[i].subject, subject)) {
                ok = 1;
                break;
            }
        }
    }
    cmq_mutex_unlock(&g_perms_lock);
    return ok;
}
