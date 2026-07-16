#define _POSIX_C_SOURCE 200809L
#include "cmq_account.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char account[CMQ_ACCOUNT_NAME_SIZE];
    cmq_account_export_t exports[CMQ_ACCOUNT_MAX_EXPORTS];
    size_t export_count;
    cmq_account_import_t imports[CMQ_ACCOUNT_MAX_IMPORTS];
    size_t import_count;
} cmq_account_perms_t;

struct cmq_account_manager {
    cmq_account_t accounts[CMQ_ACCOUNT_MAX];
    size_t count;
    cmq_account_perms_t perms[CMQ_ACCOUNT_MAX];
    size_t perms_count;
    cmq_mutex_t lock;
};

static void clear_account_perms_unlocked(cmq_account_manager_t *mgr, const char *name);

static void account_bump_epoch(cmq_account_t *a) {
    uint32_t e = __atomic_load_n(&a->epoch, __ATOMIC_RELAXED) + 1;
    if (e == 0) e = 1;
    __atomic_store_n(&a->epoch, e, __ATOMIC_RELEASE);
}

/* Plain stores race with concurrent __atomic_fetch_add on the same fields. */
static void account_clear_counters(cmq_account_t *a) {
    __atomic_store_n(&a->connections, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->subscriptions, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->messages_in, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->messages_out, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->bytes_in, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->bytes_out, 0, __ATOMIC_RELAXED);
}

/* Undo a stale fetch_add only while still soft-deleted. After reactivate,
   clear_counters owns the reset — unconditional undo would steal new-gen credits. */
static void account_undo_inc_u64(uint64_t *counter, uint64_t delta,
                                 cmq_account_t *acc) {
    if (__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE))
        return;
    uint64_t cur = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        if (__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE))
            return;
        uint64_t next = (cur > delta) ? cur - delta : 0;
        if (__atomic_compare_exchange_n(counter, &cur, next,
                                         0, __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED))
            return;
    }
}

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
    size_t nlen = strnlen(name, CMQ_ACCOUNT_NAME_SIZE);
    if (nlen == 0 || nlen >= CMQ_ACCOUNT_NAME_SIZE) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            if (!__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE)) {
                /* Reactivate: bump+clear while still inactive so stale inc
                   undo (!active window) and epoch checks see a consistent reset
                   before new CONNECT can credit the slot. */
                account_bump_epoch(&mgr->accounts[i]);
                account_clear_counters(&mgr->accounts[i]);
                __atomic_store_n(&mgr->accounts[i].active, 1, __ATOMIC_RELEASE);
            }
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    /* Prefer appending while capacity remains so soft-deleted slots keep
       their name (stable pointer for stale holders). When the table is
       full, reclaim an inactive slot for a new name — bump epoch so any
       leftover pointer fails client_account_live / get(old_name). */
    if (mgr->count >= CMQ_ACCOUNT_MAX) {
        cmq_account_t *slot = NULL;
        for (size_t i = 0; i < mgr->count; i++) {
            if (!__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE)) {
                slot = &mgr->accounts[i];
                break;
            }
        }
        if (!slot) {
            cmq_mutex_unlock(&mgr->lock);
            return -1;
        }
        clear_account_perms_unlocked(mgr, slot->name);
        account_bump_epoch(slot);
        strncpy(slot->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
        slot->name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
        account_clear_counters(slot);
        __atomic_store_n(&slot->active, 1, __ATOMIC_RELEASE);
        cmq_mutex_unlock(&mgr->lock);
        return 0;
    }
    cmq_account_t *a = &mgr->accounts[mgr->count++];
    strncpy(a->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
    a->name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
    __atomic_store_n(&a->epoch, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&a->active, 1, __ATOMIC_RELEASE);
    account_clear_counters(a);
    cmq_mutex_unlock(&mgr->lock);
    return 0;
}

int cmq_account_ensure(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return -1;
    size_t nlen = strnlen(name, CMQ_ACCOUNT_NAME_SIZE);
    if (nlen == 0 || nlen >= CMQ_ACCOUNT_NAME_SIZE) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->accounts[i].name, name) == 0) {
            int ok = __atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE);
            cmq_mutex_unlock(&mgr->lock);
            /* Soft-deleted: refuse — admin create() reactivates explicitly. */
            return ok ? 0 : -1;
        }
    }
    /* Name never used — same append/reclaim path as create(). */
    if (mgr->count >= CMQ_ACCOUNT_MAX) {
        cmq_account_t *slot = NULL;
        for (size_t i = 0; i < mgr->count; i++) {
            if (!__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE)) {
                slot = &mgr->accounts[i];
                break;
            }
        }
        if (!slot) {
            cmq_mutex_unlock(&mgr->lock);
            return -1;
        }
        clear_account_perms_unlocked(mgr, slot->name);
        account_bump_epoch(slot);
        strncpy(slot->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
        slot->name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
        account_clear_counters(slot);
        __atomic_store_n(&slot->active, 1, __ATOMIC_RELEASE);
        cmq_mutex_unlock(&mgr->lock);
        return 0;
    }
    cmq_account_t *a = &mgr->accounts[mgr->count++];
    strncpy(a->name, name, CMQ_ACCOUNT_NAME_SIZE - 1);
    a->name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
    __atomic_store_n(&a->epoch, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&a->active, 1, __ATOMIC_RELEASE);
    account_clear_counters(a);
    cmq_mutex_unlock(&mgr->lock);
    return 0;
}

int cmq_account_delete(cmq_account_manager_t *mgr, const char *name) {
    if (!mgr || !name) return -1;
    cmq_mutex_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        if (__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE) &&
            strcmp(mgr->accounts[i].name, name) == 0) {
            /* Soft-delete: bump epoch so live sessions fail client_account_live
               immediately (even across get→use TOCTOU), then clear active.
               Zero counters while inactive so stale fetch_add can undo safely. */
            account_bump_epoch(&mgr->accounts[i]);
            account_clear_counters(&mgr->accounts[i]);
            __atomic_store_n(&mgr->accounts[i].active, 0, __ATOMIC_RELEASE);
            clear_account_perms_unlocked(mgr, name);
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&mgr->lock);
    return -1;
}

cmq_account_t *cmq_account_get(cmq_account_manager_t *mgr, const char *name,
                                uint32_t *out_epoch) {
    if (!mgr || !name) return NULL;
    if (strnlen(name, CMQ_ACCOUNT_NAME_SIZE) >= CMQ_ACCOUNT_NAME_SIZE) return NULL;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_t *found = NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE) &&
            strcmp(mgr->accounts[i].name, name) == 0) {
            found = &mgr->accounts[i];
            if (out_epoch)
                *out_epoch = __atomic_load_n(&found->epoch, __ATOMIC_ACQUIRE);
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
        if (__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE)) c++;
    }
    cmq_mutex_unlock(&mgr->lock);
    return c;
}

int cmq_account_inc_connections(cmq_account_t *acc, uint32_t epoch) {
    if (!acc) return -1;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return -1;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return -1;
    __atomic_fetch_add(&acc->connections, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) {
        account_undo_inc_u64(&acc->connections, 1, acc);
        return -1;
    }
    return 0;
}
void cmq_account_dec_connections(cmq_account_t *acc, uint32_t epoch) {
    if (!acc) return;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return;
    uint64_t cur = __atomic_load_n(&acc->connections, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch)
            return;
        if (__atomic_compare_exchange_n(&acc->connections, &cur, cur - 1,
                                         0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
void cmq_account_inc_subscriptions(cmq_account_t *acc, uint32_t epoch) {
    if (!acc) return;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return;
    __atomic_fetch_add(&acc->subscriptions, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch)
        account_undo_inc_u64(&acc->subscriptions, 1, acc);
}
void cmq_account_dec_subscriptions(cmq_account_t *acc, uint32_t epoch) {
    if (!acc) return;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return;
    uint64_t cur = __atomic_load_n(&acc->subscriptions, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch)
            return;
        if (__atomic_compare_exchange_n(&acc->subscriptions, &cur, cur - 1,
                                         0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
void cmq_account_inc_msgs_in(cmq_account_t *acc, uint32_t epoch, uint64_t bytes) {
    if (!acc) return;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return;
    __atomic_fetch_add(&acc->messages_in, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&acc->bytes_in, bytes, __ATOMIC_RELAXED);
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) {
        account_undo_inc_u64(&acc->messages_in, 1, acc);
        account_undo_inc_u64(&acc->bytes_in, bytes, acc);
    }
}
void cmq_account_inc_msgs_out(cmq_account_t *acc, uint32_t epoch, uint64_t bytes) {
    if (!acc) return;
    if (!__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) return;
    __atomic_fetch_add(&acc->messages_out, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&acc->bytes_out, bytes, __ATOMIC_RELAXED);
    if (__atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != epoch) {
        account_undo_inc_u64(&acc->messages_out, 1, acc);
        account_undo_inc_u64(&acc->bytes_out, bytes, acc);
    }
}

static void clear_account_perms_unlocked(cmq_account_manager_t *mgr, const char *name) {
    for (size_t i = 0; i < mgr->perms_count; i++) {
        if (strcmp(mgr->perms[i].account, name) == 0) {
            memmove(&mgr->perms[i], &mgr->perms[i + 1],
                    (mgr->perms_count - i - 1) * sizeof(cmq_account_perms_t));
            mgr->perms_count--;
            break;
        }
    }
}

static cmq_account_perms_t *find_perms(cmq_account_manager_t *mgr, const char *account) {
    for (size_t i = 0; i < mgr->perms_count; i++) {
        if (strcmp(mgr->perms[i].account, account) == 0) return &mgr->perms[i];
    }
    return NULL;
}

static cmq_account_perms_t *find_or_create_perms(cmq_account_manager_t *mgr,
                                                   const char *account) {
    cmq_account_perms_t *p = find_perms(mgr, account);
    if (p) return p;
    if (mgr->perms_count >= CMQ_ACCOUNT_MAX) return NULL;
    p = &mgr->perms[mgr->perms_count++];
    memset(p, 0, sizeof(*p));
    size_t n = strlen(account); /* caller validated length */
    memcpy(p->account, account, n);
    p->account[n] = '\0';
    return p;
}

static int account_is_active(cmq_account_manager_t *mgr, const char *name) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (__atomic_load_n(&mgr->accounts[i].active, __ATOMIC_ACQUIRE) &&
            strcmp(mgr->accounts[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int account_name_ok(const char *name) {
    if (!name) return 0;
    size_t n = strnlen(name, CMQ_ACCOUNT_NAME_SIZE);
    return n > 0 && n < CMQ_ACCOUNT_NAME_SIZE;
}

static int acl_subject_ok(const char *subject) {
    if (!subject) return 0;
    size_t n = strnlen(subject, 256);
    if (n == 0 || n >= 256) return 0;
    if (subject[0] == '.' || subject[n - 1] == '.') return 0;
    const char *p = subject;
    while (*p) {
        const char *start = p;
        while (*p && *p != '.') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) return 0; /* empty token / ".." */
        int is_gt = (len == 1 && start[0] == '>');
        int is_star = (len == 1 && start[0] == '*');
        /* Wildcards must be whole tokens; '>' only as final token. */
        if (!is_gt && memchr(start, '>', len)) return 0;
        if (!is_star && memchr(start, '*', len)) return 0;
        if (is_gt && *p != '\0') return 0;
        if (*p == '.') p++;
    }
    return 1;
}

static int peer_account_ok(cmq_account_manager_t *mgr, const char *name) {
    if (strcmp(name, "*") == 0) return 1;
    return account_is_active(mgr, name);
}

int cmq_account_add_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *dest_account) {
    if (!mgr || !account_name_ok(account) || !acl_subject_ok(subject) ||
        !account_name_ok(dest_account))
        return -1;
    cmq_mutex_lock(&mgr->lock);
    if (!account_is_active(mgr, account) ||
        !peer_account_ok(mgr, dest_account)) {
        cmq_mutex_unlock(&mgr->lock);
        return -1;
    }
    cmq_account_perms_t *p = find_or_create_perms(mgr, account);
    if (!p) { cmq_mutex_unlock(&mgr->lock); return -1; }
    if (p->export_count >= CMQ_ACCOUNT_MAX_EXPORTS) { cmq_mutex_unlock(&mgr->lock); return -1; }
    for (size_t i = 0; i < p->export_count; i++) {
        if (strcmp(p->exports[i].subject, subject) == 0 &&
            strcmp(p->exports[i].dest_account, dest_account) == 0) {
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    cmq_account_export_t *e = &p->exports[p->export_count++];
    memset(e, 0, sizeof(*e));
    size_t slen = strlen(subject);
    memcpy(e->subject, subject, slen);
    e->subject[slen] = '\0';
    size_t dlen = strlen(dest_account);
    memcpy(e->dest_account, dest_account, dlen);
    e->dest_account[dlen] = '\0';
    e->active = 1;
    cmq_mutex_unlock(&mgr->lock);
    return 0;
}

int cmq_account_remove_export(cmq_account_manager_t *mgr, const char *account,
                               const char *subject) {
    if (!mgr || !account || !subject) return -1;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_perms_t *p = find_perms(mgr, account);
    if (!p) { cmq_mutex_unlock(&mgr->lock); return -1; }
    int removed = 0;
    for (size_t i = 0; i < p->export_count; ) {
        if (strcmp(p->exports[i].subject, subject) == 0) {
            memmove(&p->exports[i], &p->exports[i + 1],
                    (p->export_count - i - 1) * sizeof(cmq_account_export_t));
            p->export_count--;
            removed = 1;
        } else {
            i++;
        }
    }
    cmq_mutex_unlock(&mgr->lock);
    return removed ? 0 : -1;
}

size_t cmq_account_export_count(cmq_account_manager_t *mgr, const char *account) {
    if (!mgr || !account) return 0;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_perms_t *p = find_perms(mgr, account);
    size_t c = p ? p->export_count : 0;
    cmq_mutex_unlock(&mgr->lock);
    return c;
}

int cmq_account_add_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject, const char *source_account) {
    if (!mgr || !account_name_ok(account) || !acl_subject_ok(subject) ||
        !account_name_ok(source_account))
        return -1;
    cmq_mutex_lock(&mgr->lock);
    if (!account_is_active(mgr, account) ||
        !peer_account_ok(mgr, source_account)) {
        cmq_mutex_unlock(&mgr->lock);
        return -1;
    }
    cmq_account_perms_t *p = find_or_create_perms(mgr, account);
    if (!p) { cmq_mutex_unlock(&mgr->lock); return -1; }
    if (p->import_count >= CMQ_ACCOUNT_MAX_IMPORTS) { cmq_mutex_unlock(&mgr->lock); return -1; }
    for (size_t i = 0; i < p->import_count; i++) {
        if (strcmp(p->imports[i].subject, subject) == 0 &&
            strcmp(p->imports[i].source_account, source_account) == 0) {
            cmq_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    cmq_account_import_t *imp = &p->imports[p->import_count++];
    memset(imp, 0, sizeof(*imp));
    size_t slen = strlen(subject);
    memcpy(imp->subject, subject, slen);
    imp->subject[slen] = '\0';
    size_t src_len = strlen(source_account);
    memcpy(imp->source_account, source_account, src_len);
    imp->source_account[src_len] = '\0';
    imp->active = 1;
    cmq_mutex_unlock(&mgr->lock);
    return 0;
}

int cmq_account_remove_import(cmq_account_manager_t *mgr, const char *account,
                               const char *subject) {
    if (!mgr || !account || !subject) return -1;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_perms_t *p = find_perms(mgr, account);
    if (!p) { cmq_mutex_unlock(&mgr->lock); return -1; }
    int removed = 0;
    for (size_t i = 0; i < p->import_count; ) {
        if (strcmp(p->imports[i].subject, subject) == 0) {
            memmove(&p->imports[i], &p->imports[i + 1],
                    (p->import_count - i - 1) * sizeof(cmq_account_import_t));
            p->import_count--;
            removed = 1;
        } else {
            i++;
        }
    }
    cmq_mutex_unlock(&mgr->lock);
    return removed ? 0 : -1;
}

size_t cmq_account_import_count(cmq_account_manager_t *mgr, const char *account) {
    if (!mgr || !account) return 0;
    cmq_mutex_lock(&mgr->lock);
    cmq_account_perms_t *p = find_perms(mgr, account);
    size_t c = p ? p->import_count : 0;
    cmq_mutex_unlock(&mgr->lock);
    return c;
}

static int subject_match(const char *pattern, const char *subject) {
    if (strcmp(pattern, ">") == 0) return 1;
    if (strcmp(pattern, subject) == 0) return 1;
    /* Token-wise: '*' matches one token; '>' matches the rest and MUST be last. */
    const char *p = pattern, *s = subject;
    while (*p && *s) {
        const char *pe = p, *se = s;
        while (*pe && *pe != '.') pe++;
        while (*se && *se != '.') se++;
        size_t plen = (size_t)(pe - p), slen = (size_t)(se - s);
        if (plen == 1 && p[0] == '>')
            return (*pe == '\0'); /* '>' only valid as final token */
        int is_star = (plen == 1 && p[0] == '*');
        if (!is_star && (plen != slen || memcmp(p, s, plen) != 0))
            return 0;
        p = *pe ? pe + 1 : pe;
        s = *se ? se + 1 : se;
    }
    /* Lone final '>' matches any remaining subject tokens (including none). */
    if (p[0] == '>' && p[1] == '\0') return 1;
    return *p == '\0' && *s == '\0';
}

static int acct_eq_or_star(const char *rule, const char *actual) {
    return strcmp(rule, "*") == 0 || strcmp(rule, actual) == 0;
}

int cmq_account_can_import(cmq_account_manager_t *mgr, const char *account,
                            const char *subject) {
    if (!mgr || !account || !subject) return 0;
    cmq_mutex_lock(&mgr->lock);
    if (!account_is_active(mgr, account)) {
        cmq_mutex_unlock(&mgr->lock);
        return 0;
    }
    cmq_account_perms_t *p = find_perms(mgr, account);
    int ok = 1;
    if (p && p->import_count > 0) {
        ok = 0;
        for (size_t i = 0; i < p->import_count; i++) {
            if (subject_match(p->imports[i].subject, subject)) {
                ok = 1;
                break;
            }
        }
    } else {
        /* No imports: allow unless another account exports this subject to us. */
        for (size_t i = 0; i < mgr->perms_count; i++) {
            if (strcmp(mgr->perms[i].account, account) == 0) continue;
            for (size_t j = 0; j < mgr->perms[i].export_count; j++) {
                if (subject_match(mgr->perms[i].exports[j].subject, subject) &&
                    acct_eq_or_star(mgr->perms[i].exports[j].dest_account, account)) {
                    ok = 0;
                    goto out;
                }
            }
        }
    }
out:
    cmq_mutex_unlock(&mgr->lock);
    return ok;
}

int cmq_account_can_export(cmq_account_manager_t *mgr, const char *account,
                            const char *subject) {
    if (!mgr || !account || !subject) return 0;
    cmq_mutex_lock(&mgr->lock);
    if (!account_is_active(mgr, account)) {
        cmq_mutex_unlock(&mgr->lock);
        return 0;
    }
    cmq_account_perms_t *p = find_perms(mgr, account);
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
    cmq_mutex_unlock(&mgr->lock);
    return ok;
}

int cmq_account_may_deliver(cmq_account_manager_t *mgr, const char *pub_account,
                             const char *sub_account, const char *subject) {
    if (!mgr || !pub_account || !sub_account || !subject) return 0;
    cmq_mutex_lock(&mgr->lock);
    if (!account_is_active(mgr, pub_account) ||
        !account_is_active(mgr, sub_account)) {
        cmq_mutex_unlock(&mgr->lock);
        return 0;
    }
    if (strcmp(pub_account, sub_account) == 0) {
        /* Inbox replies are not export subjects — allow same-account RR. */
        if (strncmp(subject, "_INBOX.", 7) == 0) {
            cmq_mutex_unlock(&mgr->lock);
            return 1;
        }
        /* Same account still respects export allow-list (revoked mid-flight). */
        cmq_account_perms_t *p = find_perms(mgr, pub_account);
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
        cmq_mutex_unlock(&mgr->lock);
        return ok;
    }

    cmq_account_perms_t *pub = find_perms(mgr, pub_account);
    cmq_account_perms_t *sub = find_perms(mgr, sub_account);

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

    cmq_mutex_unlock(&mgr->lock);
    return (export_ok && import_ok) ? 1 : 0;
}
