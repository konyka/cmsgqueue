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
    int free_slot = -1;
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            if (!mgr->accounts[i].active) {
                /* Reactivate soft-deleted slot (stable pointer). */
                mgr->accounts[i].active = 1;
                mgr->accounts[i].connections = 0;
                mgr->accounts[i].subscriptions = 0;
                mgr->accounts[i].messages_in = 0;
                mgr->accounts[i].messages_out = 0;
                mgr->accounts[i].bytes_in = 0;
                mgr->accounts[i].bytes_out = 0;
            }
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
        if (!mgr->accounts[i].active && free_slot < 0)
            free_slot = (int)i;
    }
    cmq_account_t *a;
    if (free_slot >= 0) {
        a = &mgr->accounts[free_slot];
    } else if (mgr->count < CMQ_ACCOUNT_MAX) {
        a = &mgr->accounts[mgr->count++];
    } else {
        cmq_mutex_unlock(&mgr->lock);
        return -1;
    }
    strncpy(a->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
    a->name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
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

static void clear_account_perms(const char *name); /* after g_perms */

int cmq_account_delete(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->accounts[i].active &&
            strcmp(mgr->accounts[i].name, name) == 0) {
            /* Soft-delete: keep slot so concurrent get()+inc_* pointers stay valid. */
            mgr->accounts[i].active = 0;
            cmq_mutex_unlock(&mgr->lock);
            clear_account_perms(name);
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
        if (mgr->accounts[i].active &&
            strcmp(mgr->accounts[i].name, name) == 0) {
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
    size_t c = 0;
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->accounts[i].active) c++;
    }
    cmq_mutex_unlock(&mgr->lock);
    return c;
}

void cmq_account_inc_connections(cmq_account_t *acc) {
    if (acc && acc->active)
        __atomic_fetch_add(&acc->connections, 1, __ATOMIC_RELAXED);
}
void cmq_account_dec_connections(cmq_account_t *acc) {
    if (!acc || !acc->active) return;
    uint64_t cur = __atomic_load_n(&acc->connections, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&acc->connections, &cur, cur - 1,
                                         0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
void cmq_account_inc_subscriptions(cmq_account_t *acc) {
    if (acc && acc->active)
        __atomic_fetch_add(&acc->subscriptions, 1, __ATOMIC_RELAXED);
}
void cmq_account_dec_subscriptions(cmq_account_t *acc) {
    if (!acc || !acc->active) return;
    uint64_t cur = __atomic_load_n(&acc->subscriptions, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&acc->subscriptions, &cur, cur - 1,
                                         0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
void cmq_account_inc_msgs_in(cmq_account_t *acc, uint64_t bytes) {
    if (!acc || !acc->active) return;
    __atomic_fetch_add(&acc->messages_in, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&acc->bytes_in, bytes, __ATOMIC_RELAXED);
}
void cmq_account_inc_msgs_out(cmq_account_t *acc, uint64_t bytes) {
    if (!acc || !acc->active) return;
    __atomic_fetch_add(&acc->messages_out, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&acc->bytes_out, bytes, __ATOMIC_RELAXED);
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

static void perms_lock_init(void) {
    cmq_mutex_init(&g_perms_lock);
}

static void ensure_perms_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, perms_lock_init);
}

static void clear_account_perms(const char *name) {
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    for (size_t i = 0; i < g_perms_count; i++) {
        if (strcmp(g_perms[i].account, name) == 0) {
            memmove(&g_perms[i], &g_perms[i + 1],
                    (g_perms_count - i - 1) * sizeof(cmq_account_perms_t));
            g_perms_count--;
            break;
        }
    }
    cmq_mutex_unlock(&g_perms_lock);
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
    memset(p, 0, sizeof(*p));
    strncpy(p->account, account, CMQ_ACCOUNT_NAME_SIZE - 1);
    p->account[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
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
    memset(e, 0, sizeof(*e));
    strncpy(e->subject, subject, sizeof(e->subject) - 1);
    e->subject[sizeof(e->subject) - 1] = '\0';
    strncpy(e->dest_account, dest_account, CMQ_ACCOUNT_NAME_SIZE - 1);
    e->dest_account[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
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
    memset(imp, 0, sizeof(*imp));
    strncpy(imp->subject, subject, sizeof(imp->subject) - 1);
    imp->subject[sizeof(imp->subject) - 1] = '\0';
    strncpy(imp->source_account, source_account, CMQ_ACCOUNT_NAME_SIZE - 1);
    imp->source_account[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
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
    /* Token-wise: '*' matches one token; '>' matches rest (NATS-like). */
    const char *p = pattern, *s = subject;
    while (*p && *s) {
        if (*p == '>') return 1;
        const char *pe = p, *se = s;
        while (*pe && *pe != '.') pe++;
        while (*se && *se != '.') se++;
        size_t plen = (size_t)(pe - p), slen = (size_t)(se - s);
        int is_star = (plen == 1 && p[0] == '*');
        if (!is_star && (plen != slen || memcmp(p, s, plen) != 0))
            return 0;
        p = *pe ? pe + 1 : pe;
        s = *se ? se + 1 : se;
    }
    if (*p == '>') return 1;
    return *p == '\0' && *s == '\0';
}

int cmq_account_can_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject) {
    (void)mgr;
    if (!account || !subject) return 0;
    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);
    cmq_account_perms_t *p = find_perms(account);
    /* No ACL entry or empty import list → allow (open default). */
    int ok = 1;
    if (p && p->import_count > 0) {
        ok = 0;
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
    int ok = 1;
    if (p && p->export_count > 0) {
        ok = 0;
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

static int acct_eq_or_star(const char *rule, const char *actual) {
    return strcmp(rule, "*") == 0 || strcmp(rule, actual) == 0;
}

int cmq_account_may_deliver(cmq_account_manager_t *mgr, const char *pub_account,
                             const char *sub_account, const char *subject) {
    (void)mgr;
    if (!pub_account || !sub_account || !subject) return 0;
    if (strcmp(pub_account, sub_account) == 0) return 1;

    ensure_perms_init();
    cmq_mutex_lock(&g_perms_lock);

    cmq_account_perms_t *pub = find_perms(pub_account);
    cmq_account_perms_t *sub = find_perms(sub_account);

    int export_ok = 0;
    if (pub && pub->export_count > 0) {
        for (size_t i = 0; i < pub->export_count; i++) {
            if (subject_match(pub->exports[i].subject, subject) &&
                acct_eq_or_star(pub->exports[i].dest_account, sub_account)) {
                export_ok = 1;
                break;
            }
        }
    }

    int import_ok = 0;
    if (sub && sub->import_count > 0) {
        for (size_t i = 0; i < sub->import_count; i++) {
            if (subject_match(sub->imports[i].subject, subject) &&
                acct_eq_or_star(sub->imports[i].source_account, pub_account)) {
                import_ok = 1;
                break;
            }
        }
    }

    cmq_mutex_unlock(&g_perms_lock);
    /* Cross-account requires matching export and import (default deny). */
    return (export_ok && import_ok) ? 1 : 0;
}
