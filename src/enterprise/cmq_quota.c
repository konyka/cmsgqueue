#define _POSIX_C_SOURCE 200809L
#include "cmq_quota.h"
#include "cmq_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* P1: fixed-slot open-address + atomic counters. Same fix as
 * cmq_subject_rl; see v0.5.1.bundle.md B2. */

#define CMQ_QUOTA_SLOTS 1024
#define CMQ_QUOTA_MAX_PROBE 16
#define CMQ_QUOTA_KEY_MAX  255

struct cmq_quota_slot {
    char account[CMQ_QUOTA_KEY_MAX + 1];
    cmq_atomic_u64 window_start_ms;
    cmq_atomic_u32 msgs;
    cmq_atomic_u64 bytes;
    cmq_atomic_u32 connects;
};

struct cmq_quota {
    uint32_t max_msgs;
    uint32_t max_bytes;
    uint32_t max_connects;
    struct cmq_quota_slot slots[CMQ_QUOTA_SLOTS];
};

static uint64_t fnv1a(const char *s, size_t max_len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < max_len && s[i]; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

cmq_quota_t *cmq_quota_create(uint32_t max_msgs, uint32_t max_bytes,
                                uint32_t max_connects) {
    cmq_quota_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->max_msgs = max_msgs;
    q->max_bytes = max_bytes;
    q->max_connects = max_connects;
    return q;
}

void cmq_quota_free(cmq_quota_t *q) {
    free(q);
}

static struct cmq_quota_slot *find_or_claim(cmq_quota_t *q,
                                             const char *acc) {
    uint64_t h = fnv1a(acc, CMQ_QUOTA_KEY_MAX);
    uint32_t idx = (uint32_t)(h % CMQ_QUOTA_SLOTS);

    for (uint32_t probe = 0; probe < CMQ_QUOTA_MAX_PROBE; probe++) {
        uint32_t i = (idx + probe) % CMQ_QUOTA_SLOTS;
        struct cmq_quota_slot *s = &q->slots[i];

        char key_buf[CMQ_QUOTA_KEY_MAX + 1];
        memcpy(key_buf, s->account, CMQ_QUOTA_KEY_MAX + 1);

        if (key_buf[0] == '\0') {
            cmq_u32_t zero = 0;
            if (cmq_atomic_cas_u32(&s->msgs, &zero, 0,
                                    CMQ_ATOMIC_ACQ_REL)) {
                size_t slen = strnlen(acc, CMQ_QUOTA_KEY_MAX);
                memcpy(s->account, acc, slen);
                s->account[slen] = '\0';
                cmq_atomic_store_u64(&s->window_start_ms, now_ms(),
                                      CMQ_ATOMIC_RELAXED);
                return s;
            }
            continue;
        }

        if (strncmp(key_buf, acc, CMQ_QUOTA_KEY_MAX) != 0) {
            continue;
        }
        return s;
    }
    return NULL;
}

int cmq_quota_check_publish(cmq_quota_t *q, const char *account,
                            size_t msg_len) {
    if (!q || !account) return 1;
    if (q->max_msgs == 0 && q->max_bytes == 0) return 1;

    struct cmq_quota_slot *s = find_or_claim(q, account);
    if (!s) return 1;  /* fail-open */

    uint64_t now = now_ms();
    uint64_t wstart = cmq_atomic_load_u64(&s->window_start_ms,
                                           CMQ_ATOMIC_RELAXED);
    if (now - wstart >= 1000) {
        cmq_atomic_store_u64(&s->window_start_ms, now,
                              CMQ_ATOMIC_RELAXED);
        cmq_atomic_store_u32(&s->msgs, 0, CMQ_ATOMIC_RELAXED);
        cmq_atomic_store_u64(&s->bytes, 0, CMQ_ATOMIC_RELAXED);
    }

    /* The claim-CAS already counted this publish (msgs=1). Subsequent
     * admits increment one more. So: load, loop while (cur < max_msgs)
     * with CAS. cur==max means saturated. */
    cmq_u32_t cur = cmq_atomic_load_u32(&s->msgs, CMQ_ATOMIC_RELAXED);
    while (cur < q->max_msgs) {
        if (cmq_atomic_cas_u32(&s->msgs, &cur, cur + 1,
                                CMQ_ATOMIC_ACQ_REL)) {
            return 1;
        }
    }
    if (q->max_msgs > 0) return 0;

    if (q->max_bytes > 0) {
        cmq_u64_t b = cmq_atomic_load_u64(&s->bytes, CMQ_ATOMIC_RELAXED);
        while (b + msg_len <= q->max_bytes) {
            cmq_u64_t old = b;
            if (atomic_compare_exchange_weak_explicit(
                    &s->bytes, &old, b + msg_len,
                    CMQ_ATOMIC_ACQ_REL, CMQ_ATOMIC_RELAXED)) {
                return 1;
            }
            b = old;
        }
        return 0;
    }
    return 1;
}

int cmq_quota_check_connect(cmq_quota_t *q, const char *account) {
    if (!q || !account) return 1;
    if (q->max_connects == 0) return 1;

    struct cmq_quota_slot *s = find_or_claim(q, account);
    if (!s) return 1;

    uint64_t now = now_ms();
    uint64_t wstart = cmq_atomic_load_u64(&s->window_start_ms,
                                           CMQ_ATOMIC_RELAXED);
    if (now - wstart >= 1000) {
        cmq_atomic_store_u64(&s->window_start_ms, now,
                              CMQ_ATOMIC_RELAXED);
        cmq_atomic_store_u32(&s->connects, 0, CMQ_ATOMIC_RELAXED);
    }

    cmq_u32_t cur = cmq_atomic_load_u32(&s->connects, CMQ_ATOMIC_RELAXED);
    while (cur < q->max_connects) {
        if (cmq_atomic_cas_u32(&s->connects, &cur, cur + 1,
                                CMQ_ATOMIC_ACQ_REL)) {
            return 1;
        }
    }
    return 0;
}