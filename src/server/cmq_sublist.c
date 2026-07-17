#define _POSIX_C_SOURCE 200809L
#include "cmq_sublist.h"
#include <stdlib.h>
#include <string.h>

typedef struct cmq_sl_node {
    struct cmq_sl_node *children;
    struct cmq_sl_node *next;
    char *token;
    int is_pwc;
    int is_fwc;
    void **subs;
    size_t sub_count;
    size_t sub_cap;
} cmq_sl_node_t;

struct cmq_sublist {
    cmq_sl_node_t root;
    cmq_rwlock_t lock;
    size_t count;
};

static cmq_sl_node_t *cmq_sl_node_create(const char *token, int is_pwc, int is_fwc) {
    cmq_sl_node_t *n = calloc(1, sizeof(cmq_sl_node_t));
    if (!n) return NULL;
    if (token) {
        n->token = strdup(token);
    }
    n->is_pwc = is_pwc;
    n->is_fwc = is_fwc;
    return n;
}

static void cmq_sl_node_destroy(cmq_sl_node_t *node) {
    if (!node) return;
    cmq_sl_node_t *child = node->children;
    while (child) {
        cmq_sl_node_t *next = child->next;
        cmq_sl_node_destroy(child);
        child = next;
    }
    free(node->token);
    free(node->subs);
    free(node);
}

static int validate_subject(const char *subject) {
    if (!subject || subject[0] == '\0') return -1;
    size_t len = strlen(subject);
    if (subject[len - 1] == '.') return -1;
    if (subject[0] == '.') return -1;

    int has_dot_dot = 0;
    for (size_t i = 0; i < len; i++) {
        if (subject[i] == '.' && i + 1 < len && subject[i + 1] == '.') {
            has_dot_dot = 1;
            break;
        }
    }
    if (has_dot_dot) return -1;

    const char *p = subject;
    while (*p) {
        if (*p == '>') {
            const char *next = p + 1;
            if (*next != '\0' && *next != '.') return -1;
            if (*next == '.') return -1;
        }
        p++;
    }
    return 0;
}

static int tokenize(const char *subject, char tokens[][256], int *ntokens) {
    *ntokens = 0;
    const char *p = subject;
    while (*p && *ntokens < 64) {
        const char *start = p;
        while (*p && *p != '.') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) return -1;
        if (len > 255) return -1;
        memcpy(tokens[*ntokens], start, len);
        tokens[*ntokens][len] = '\0';
        (*ntokens)++;
        if (*p == '.') p++;
    }
    /* Remaining tokens beyond the 64-token cap are invalid. */
    if (*p) return -1;
    return 0;
}

int cmq_sublist_subject_valid(const char *subject) {
    if (validate_subject(subject) != 0) return -1;
    char tokens[64][256];
    int ntokens;
    return tokenize(subject, tokens, &ntokens);
}

int cmq_sublist_publish_subject_valid(const char *subject) {
    if (cmq_sublist_subject_valid(subject) != 0) return -1;
    for (const char *p = subject; *p; p++) {
        if (*p == '*' || *p == '>') return -1;
    }
    return 0;
}

static cmq_sl_node_t *find_child(cmq_sl_node_t *parent, const char *token, int is_pwc, int is_fwc) {
    cmq_sl_node_t *child = parent->children;
    while (child) {
        if (is_pwc && child->is_pwc) return child;
        if (is_fwc && child->is_fwc) return child;
        if (!is_pwc && !is_fwc && !child->is_pwc && !child->is_fwc &&
            child->token && strcmp(child->token, token) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static cmq_sl_node_t *add_child(cmq_sl_node_t *parent, const char *token, int is_pwc, int is_fwc) {
    cmq_sl_node_t *child = cmq_sl_node_create(token, is_pwc, is_fwc);
    if (!child) return NULL;
    child->next = parent->children;
    parent->children = child;
    return child;
}

static void unlink_child(cmq_sl_node_t *parent, cmq_sl_node_t *child) {
    cmq_sl_node_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Drop empty nodes created during a failed insert (OOM rollback). */
static void rollback_created(cmq_sl_node_t **created, cmq_sl_node_t **parents,
                             int ncreated) {
    for (int j = ncreated - 1; j >= 0; j--) {
        if (created[j]->sub_count != 0 || created[j]->children != NULL)
            break;
        unlink_child(parents[j], created[j]);
        cmq_sl_node_destroy(created[j]);
    }
}

static int node_add_sub(cmq_sl_node_t *node, void *data) {
    if (node->sub_count >= node->sub_cap) {
        size_t new_cap = node->sub_cap == 0 ? 4 : node->sub_cap * 2;
        void **new_subs = realloc(node->subs, new_cap * sizeof(void *));
        if (!new_subs) return -1;
        node->subs = new_subs;
        node->sub_cap = new_cap;
    }
    node->subs[node->sub_count++] = data;
    return 0;
}

static int node_remove_sub(cmq_sl_node_t *node, void *data) {
    for (size_t i = 0; i < node->sub_count; i++) {
        if (node->subs[i] == data) {
            node->subs[i] = node->subs[node->sub_count - 1];
            node->sub_count--;
            return 0;
        }
    }
    return -1;
}

cmq_sublist_t *cmq_sublist_create(void) {
    cmq_sublist_t *sl = calloc(1, sizeof(cmq_sublist_t));
    if (!sl) return NULL;
    cmq_rwlock_init(&sl->lock);
    return sl;
}

static void node_free_data(cmq_sl_node_t *node) {
    if (!node) return;
    for (size_t i = 0; i < node->sub_count; i++) {
        free(node->subs[i]);
    }
    cmq_sl_node_t *child = node->children;
    while (child) {
        node_free_data(child);
        child = child->next;
    }
}

void cmq_sublist_free_data(cmq_sublist_t *sl) {
    if (!sl) return;
    cmq_rwlock_wrlock(&sl->lock);
    node_free_data(&sl->root);
    cmq_rwlock_unlock(&sl->lock);
}

void cmq_sublist_destroy(cmq_sublist_t *sl) {
    if (!sl) return;
    cmq_rwlock_wrlock(&sl->lock);
    cmq_sl_node_t *child = sl->root.children;
    sl->root.children = NULL;
    sl->count = 0;
    while (child) {
        cmq_sl_node_t *next = child->next;
        cmq_sl_node_destroy(child);
        child = next;
    }
    cmq_rwlock_unlock(&sl->lock);
    cmq_rwlock_destroy(&sl->lock);
    free(sl);
}

int cmq_sublist_insert(cmq_sublist_t *sl, const char *subject, void *data) {
    if (!sl || validate_subject(subject) != 0) return -1;

    char tokens[64][256];
    int ntokens;
    if (tokenize(subject, tokens, &ntokens) != 0) return -1;

    cmq_rwlock_wrlock(&sl->lock);

    cmq_sl_node_t *current = &sl->root;
    cmq_sl_node_t *created[64];
    cmq_sl_node_t *created_parent[64];
    int ncreated = 0;
    for (int i = 0; i < ntokens; i++) {
        int is_pwc = (strcmp(tokens[i], "*") == 0);
        int is_fwc = (strcmp(tokens[i], ">") == 0);

        cmq_sl_node_t *child = find_child(current, tokens[i], is_pwc, is_fwc);
        if (!child) {
            child = add_child(current, is_pwc ? NULL : (is_fwc ? NULL : tokens[i]), is_pwc, is_fwc);
            if (!child) {
                rollback_created(created, created_parent, ncreated);
                cmq_rwlock_unlock(&sl->lock);
                return -1;
            }
            if (ncreated < 64) {
                created[ncreated] = child;
                created_parent[ncreated] = current;
                ncreated++;
            }
        }
        current = child;
    }

    int rc = node_add_sub(current, data);
    if (rc == 0) {
        sl->count++;
    } else {
        rollback_created(created, created_parent, ncreated);
    }
    cmq_rwlock_unlock(&sl->lock);
    return rc;
}

int cmq_sublist_remove(cmq_sublist_t *sl, const char *subject, void *data) {
    if (!sl || !data || validate_subject(subject) != 0) return -1;

    char tokens[64][256];
    int ntokens;
    if (tokenize(subject, tokens, &ntokens) != 0) return -1;

    cmq_rwlock_wrlock(&sl->lock);

    cmq_sl_node_t *parents[64];
    int nparents = 0;
    cmq_sl_node_t *current = &sl->root;
    for (int i = 0; i < ntokens; i++) {
        int is_pwc = (strcmp(tokens[i], "*") == 0);
        int is_fwc = (strcmp(tokens[i], ">") == 0);
        cmq_sl_node_t *child = find_child(current, tokens[i], is_pwc, is_fwc);
        if (!child) {
            cmq_rwlock_unlock(&sl->lock);
            return -1;
        }
        if (nparents < 64)
            parents[nparents++] = current;
        current = child;
    }

    int rc = node_remove_sub(current, data);
    if (rc == 0 && sl->count > 0) sl->count--;
    /* Prune empty trie nodes (keep root). */
    if (rc == 0) {
        cmq_sl_node_t *node = current;
        for (int i = nparents - 1; i >= 0; i--) {
            if (node->sub_count > 0 || node->children != NULL)
                break;
            unlink_child(parents[i], node);
            cmq_sl_node_destroy(node);
            node = parents[i];
        }
    }
    cmq_rwlock_unlock(&sl->lock);
    return rc;
}

static int result_append(cmq_sublist_result_t *result, void *data) {
    if (result->count >= result->cap) {
        size_t new_cap = result->cap == 0 ? 8 : result->cap * 2;
        void **new_entries = realloc(result->entries, new_cap * sizeof(void *));
        if (!new_entries) return -1;
        result->entries = new_entries;
        result->cap = new_cap;
    }
    result->entries[result->count++] = data;
    return 0;
}

static int match_recursive(cmq_sl_node_t *node, char tokens[][256], int ntokens, int depth,
                            cmq_sublist_result_t *result) {
    if (depth >= ntokens) {
        for (size_t i = 0; i < node->sub_count; i++) {
            if (result_append(result, node->subs[i]) != 0) return -1;
        }
        /* NATS: foo.> matches foo (zero remaining tokens after prefix). */
        for (cmq_sl_node_t *ch = node->children; ch; ch = ch->next) {
            if (!ch->is_fwc) continue;
            for (size_t i = 0; i < ch->sub_count; i++) {
                if (result_append(result, ch->subs[i]) != 0) return -1;
            }
        }
        return 0;
    }

    cmq_sl_node_t *child = node->children;
    while (child) {
        if (child->is_fwc) {
            for (size_t i = 0; i < child->sub_count; i++) {
                if (result_append(result, child->subs[i]) != 0) return -1;
            }
        } else if (child->is_pwc) {
            if (depth + 1 <= ntokens) {
                if (match_recursive(child, tokens, ntokens, depth + 1, result) != 0)
                    return -1;
            }
        } else if (child->token && strcmp(child->token, tokens[depth]) == 0) {
            if (match_recursive(child, tokens, ntokens, depth + 1, result) != 0)
                return -1;
        }
        child = child->next;
    }
    return 0;
}

int cmq_sublist_match(cmq_sublist_t *sl, const char *subject, cmq_sublist_result_t *result) {
    if (!sl || !subject || !result) return -1;
    memset(result, 0, sizeof(*result));

    char tokens[64][256];
    int ntokens;
    if (tokenize(subject, tokens, &ntokens) != 0) return -1;

    cmq_rwlock_rdlock(&sl->lock);
    int rc = match_recursive(&sl->root, tokens, ntokens, 0, result);
    cmq_rwlock_unlock(&sl->lock);
    if (rc != 0) {
        cmq_sublist_result_free(result);
        memset(result, 0, sizeof(*result));
        return -1;
    }
    return 0;
}

void cmq_sublist_result_free(cmq_sublist_result_t *result) {
    if (!result) return;
    free(result->entries);
    result->entries = NULL;
    result->count = 0;
    result->cap = 0;
}

size_t cmq_sublist_count(cmq_sublist_t *sl) {
    if (!sl) return 0;
    cmq_rwlock_rdlock(&sl->lock);
    size_t c = sl->count;
    cmq_rwlock_unlock(&sl->lock);
    return c;
}
