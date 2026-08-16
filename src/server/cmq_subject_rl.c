#define _POSIX_C_SOURCE 200809L
#include "cmq_subject_rl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CMQ_SUBJECT_RL_SLOTS 4096

struct cmq_subject_bucket {
    char subject[256];
    uint64_t window_start_ms;
    uint32_t count;
    struct cmq_subject_bucket *next;
};

struct cmq_subject_rl {
    uint32_t max_per_sec;
    struct cmq_subject_bucket *buckets;
};

cmq_subject_rl_t *cmq_subject_rl_create(uint32_t max_msgs_per_sec) {
    cmq_subject_rl_t *rl = calloc(1, sizeof(*rl));
    if (!rl) return NULL;
    rl->max_per_sec = max_msgs_per_sec;
    return rl;
}

void cmq_subject_rl_free(cmq_subject_rl_t *rl) {
    if (!rl) return;
    struct cmq_subject_bucket *b = rl->buckets;
    while (b) {
        struct cmq_subject_bucket *n = b->next;
        free(b);
        b = n;
    }
    free(rl);
}

static struct cmq_subject_bucket *find_or_create(cmq_subject_rl_t *rl,
                                                  const char *subject) {
    struct cmq_subject_bucket *b = rl->buckets;
    while (b) {
        if (strcmp(b->subject, subject) == 0) return b;
        b = b->next;
    }
    b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    snprintf(b->subject, sizeof(b->subject), "%s", subject);
    b->next = rl->buckets;
    rl->buckets = b;
    return b;
}

int cmq_subject_rl_check(cmq_subject_rl_t *rl, const char *subject) {
    if (!rl || !subject || rl->max_per_sec == 0) return 1;
    struct cmq_subject_bucket *b = find_or_create(rl, subject);
    if (!b) return 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL;
    if (now_ms - b->window_start_ms >= 1000) {
        b->window_start_ms = now_ms;
        b->count = 0;
    }
    if (b->count >= rl->max_per_sec) return 0;
    b->count++;
    return 1;
}
