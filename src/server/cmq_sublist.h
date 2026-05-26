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
int cmq_sublist_remove(cmq_sublist_t *sl, const char *subject);
void cmq_sublist_match(cmq_sublist_t *sl, const char *subject, cmq_sublist_result_t *result);
void cmq_sublist_result_free(cmq_sublist_result_t *result);
size_t cmq_sublist_count(cmq_sublist_t *sl);

#endif
