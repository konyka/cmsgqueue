#ifndef CMQ_SUBLIST_H
#define CMQ_SUBLIST_H

#include <stddef.h>
#include <stdint.h>
#include "cmq_thread.h"

typedef struct cmq_sublist cmq_sublist_t;

typedef struct {
    void **entries;
    size_t count;
    size_t cap;
} cmq_sublist_result_t;

cmq_sublist_t *cmq_sublist_create(void);
void cmq_sublist_destroy(cmq_sublist_t *sl);
void cmq_sublist_free_data(cmq_sublist_t *sl);

int cmq_sublist_insert(cmq_sublist_t *sl, const char *subject, void *data);
/* Remove the exact data pointer under subject. Returns 0 on success, -1 if not found.
   Does not free data — caller owns the pointer. */
int cmq_sublist_remove(cmq_sublist_t *sl, const char *subject, void *data);
/* 0 if subject is legal for publish/subscribe (tokens, dots, wildcards). */
int cmq_sublist_subject_valid(const char *subject);
/* Match subject against the trie. Returns 0 on success, -1 on OOM (result cleared).
   Invalid subjects should be rejected via cmq_sublist_subject_valid first. */
int cmq_sublist_match(cmq_sublist_t *sl, const char *subject, cmq_sublist_result_t *result);
void cmq_sublist_result_free(cmq_sublist_result_t *result);
size_t cmq_sublist_count(cmq_sublist_t *sl);

#endif
