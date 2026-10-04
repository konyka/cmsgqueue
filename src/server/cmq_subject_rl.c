#define _POSIX_C_SOURCE 200809L
#include "cmq_subject_rl.h"
#include "cmq_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* P1: replace the linked-list bucket table with a fixed-slot open-address
 * hash table + atomic counters. The previous design had two races:
 *   1. find_or_create mutated rl->buckets without a lock, so two workers
 *      could create two buckets for the same subject, or lose a node.
 *   2. count++ was non-atomic, so check-then-increment could admit more
 *      than max_per_sec messages per window.
 *
 * Fixed-slot open addressing with FNV-1a hash + bounded linear probing.
 * Per-slot state is updated atomically; per-subject lookup is O(1)
 * amortized. Collisions share a slot (fail-open on probe exhaustion). */

#define CMQ_SUBJECT_RL_SLOTS 4096
#define CMQ_SUBJECT_RL_MAX_PROBE 32
#define CMQ_SUBJECT_RL_KEY_MAX  255

struct cmq_subject_slot {
    char key[CMQ_SUBJECT_RL_KEY_MAX + 1];
    cmq_atomic_u64 window_start_ms;
    cmq_atomic_u32 count;
};

struct cmq_subject_rl {
    uint32_t max_per_sec;
    struct cmq_subject_slot slots[CMQ_SUBJECT_RL_SLOTS];
};

static uint64_t fnv1a(const char *s, size_t max_len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < max_len && s[i]; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

cmq_subject_rl_t *cmq_subject_rl_create(uint32_t max_msgs_per_sec) {
    cmq_subject_rl_t *rl = calloc(1, sizeof(*rl));
    if (!rl) return NULL;
    rl->max_per_sec = max_msgs_per_sec;
    return rl;
}

void cmq_subject_rl_free(cmq_subject_rl_t *rl) {
    free(rl);
}

uint32_t cmq_subject_rl_limit(const cmq_subject_rl_t *rl) {
    return rl ? rl->max_per_sec : 0;
}

int cmq_subject_rl_reload(cmq_subject_rl_t **rl, int max_per_sec) {
    if (!rl) return -1;
    if (max_per_sec < 0 || max_per_sec > 1000000) return -1;
    if (max_per_sec == 0) return 0;
    if (!*rl) {
        *rl = cmq_subject_rl_create((uint32_t)max_per_sec);
        return *rl ? 0 : -1;
    }
    (*rl)->max_per_sec = (uint32_t)max_per_sec;
    return 0;
}

int cmq_subject_rl_check(cmq_subject_rl_t *rl, const char *subject) {
    if (!rl || !subject || rl->max_per_sec == 0) return 1;

    uint64_t h = fnv1a(subject, CMQ_SUBJECT_RL_KEY_MAX);
    uint32_t idx = (uint32_t)(h % CMQ_SUBJECT_RL_SLOTS);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL;

    for (uint32_t probe = 0; probe < CMQ_SUBJECT_RL_MAX_PROBE; probe++) {
        uint32_t i = (idx + probe) % CMQ_SUBJECT_RL_SLOTS;
        struct cmq_subject_slot *s = &rl->slots[i];

        char key_buf[CMQ_SUBJECT_RL_KEY_MAX + 1];
        memcpy(key_buf, s->key, CMQ_SUBJECT_RL_KEY_MAX + 1);

        if (key_buf[0] == '\0') {
            cmq_u32_t zero = 0;
            if (cmq_atomic_cas_u32(&s->count, &zero, 1,
                                    CMQ_ATOMIC_ACQ_REL)) {
                size_t slen = strnlen(subject, CMQ_SUBJECT_RL_KEY_MAX);
                memcpy(s->key, subject, slen);
                s->key[slen] = '\0';
                cmq_atomic_store_u64(&s->window_start_ms, now_ms,
                                      CMQ_ATOMIC_RELAXED);
                return 1;
            }
            continue;
        }

        if (strncmp(key_buf, subject, CMQ_SUBJECT_RL_KEY_MAX) != 0) {
            continue;
        }

        uint64_t wstart = cmq_atomic_load_u64(&s->window_start_ms,
                                               CMQ_ATOMIC_RELAXED);
        if (now_ms - wstart >= 1000) {
            cmq_atomic_store_u64(&s->window_start_ms, now_ms,
                                  CMQ_ATOMIC_RELAXED);
            cmq_atomic_store_u32(&s->count, 0, CMQ_ATOMIC_RELAXED);
        }

        cmq_u32_t cur = cmq_atomic_load_u32(&s->count, CMQ_ATOMIC_RELAXED);
        while (cur < rl->max_per_sec) {
            if (cmq_atomic_cas_u32(&s->count, &cur, cur + 1,
                                    CMQ_ATOMIC_ACQ_REL)) {
                return 1;
            }
        }
        return 0;
    }
    return 1;
}