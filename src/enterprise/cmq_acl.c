#define _POSIX_C_SOURCE 200809L
#include "cmq_acl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMQ_ACL_MAX_PATTERNS 1024

struct cmq_pattern {
    char *pat;
    int is_pwc;  /* single-token wildcard "*" */
    int is_fwc;  /* full wildcard ">" */
    int tokens;  /* number of '.'-separated tokens (without wildcards) */
};

struct cmq_acl {
    struct cmq_pattern allow[CMQ_ACL_MAX_PATTERNS];
    int n_allow;
    struct cmq_pattern deny[CMQ_ACL_MAX_PATTERNS];
    int n_deny;
};

static int compile_pattern(const char *src, struct cmq_pattern *out) {
    out->pat = strdup(src);
    if (!out->pat) return -1;
    out->is_pwc = 0;
    out->is_fwc = 0;
    out->tokens = 1;
    for (const char *p = src; *p; p++) {
        if (p[0] == '*' && p[1] == '\0') out->is_pwc = 1;
        if (p[0] == '>' && p[1] == '\0') out->is_fwc = 1;
        if (*p == '.') out->tokens++;
    }
    return 0;
}

cmq_acl_t *cmq_acl_create(void) {
    cmq_acl_t *acl = calloc(1, sizeof(*acl));
    return acl;
}

void cmq_acl_free(cmq_acl_t *acl) {
    if (!acl) return;
    for (int i = 0; i < acl->n_allow; i++) free(acl->allow[i].pat);
    for (int i = 0; i < acl->n_deny; i++) free(acl->deny[i].pat);
    free(acl);
}

int cmq_acl_allow(cmq_acl_t *acl, const char *pattern) {
    if (!acl || acl->n_allow >= CMQ_ACL_MAX_PATTERNS) return -1;
    return compile_pattern(pattern, &acl->allow[acl->n_allow++]);
}

int cmq_acl_deny(cmq_acl_t *acl, const char *pattern) {
    if (!acl || acl->n_deny >= CMQ_ACL_MAX_PATTERNS) return -1;
    return compile_pattern(pattern, &acl->deny[acl->n_deny++]);
}

static int token_count(const char *s) {
    int n = 1;
    for (const char *p = s; *p; p++) if (*p == '.') n++;
    return n;
}

static int match_pattern(const struct cmq_pattern *p, const char *subject) {
    int subj_tokens = token_count(subject);
    if (p->is_fwc) {
        /* Pattern ends with ".>" — match if subject has at least
         * as many tokens as the pattern prefix. */
        size_t plen = strlen(p->pat) - 2; /* strip ".<" -- actually ".>" */
        /* plen is position of '.', so prefix length is plen */
        if (subj_tokens < p->tokens) return 0;
        /* Compare prefix */
        if (strncmp(p->pat, subject, plen) != 0) return 0;
        if (p->pat[plen] != '.') return 0;
        return 1;
    }
    if (p->is_pwc) {
        /* Single-token wildcard: pattern is X.* (or X.* etc.) */
        if (subj_tokens != p->tokens) return 0;
        /* Match the literal part up to the last '.' */
        const char *last_dot = strrchr(p->pat, '.');
        if (!last_dot || last_dot[1] != '*') return 0;
        size_t literal_len = (size_t)(last_dot - p->pat);
        if (strncmp(p->pat, subject, literal_len) != 0) return 0;
        if (subject[literal_len] != '.') return 0;
        return 1;
    }
    return strcmp(p->pat, subject) == 0;
}

int cmq_acl_check(cmq_acl_t *acl, const char *subject) {
    if (!acl || !subject) return 1;
    /* Deny-list wins. */
    for (int i = 0; i < acl->n_deny; i++) {
        if (match_pattern(&acl->deny[i], subject)) return 0;
    }
    /* If allow-list is set, subject must match. */
    if (acl->n_allow > 0) {
        for (int i = 0; i < acl->n_allow; i++) {
            if (match_pattern(&acl->allow[i], subject)) return 1;
        }
        return 0;
    }
    return 1;
}
