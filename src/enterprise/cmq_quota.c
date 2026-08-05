#define _POSIX_C_SOURCE 200809L
#include "cmq_quota.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cmq_account_bucket {
    char account[256];
    uint64_t window_start_ms;
    uint32_t msgs;
    uint64_t bytes;
    uint32_t connects;
    struct cmq_account_bucket *next;
};

struct cmq_quota {
    uint32_t max_msgs;
    uint32_t max_bytes;
    uint32_t max_connects;
    struct cmq_account_bucket *buckets;
};

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
    if (!q) return;
    struct cmq_account_bucket *b = q->buckets;
    while (b) {
        struct cmq_account_bucket *n = b->next;
        free(b);
        b = n;
    }
    free(q);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static struct cmq_account_bucket *find_or_create(cmq_quota_t *q, const char *acc) {
    struct cmq_account_bucket *b = q->buckets;
    while (b) {
        if (strcmp(b->account, acc) == 0) return b;
        b = b->next;
    }
    b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    snprintf(b->account, sizeof(b->account), "%s", acc);
    b->next = q->buckets;
    q->buckets = b;
    return b;
}

int cmq_quota_check_publish(cmq_quota_t *q, const char *account,
                            size_t msg_len) {
    if (!q || !account) return 1;
    if (q->max_msgs == 0 && q->max_bytes == 0) return 1;
    struct cmq_account_bucket *b = find_or_create(q, account);
    if (!b) return 1;
    uint64_t now = now_ms();
    if (now - b->window_start_ms >= 1000) {
        b->window_start_ms = now;
        b->msgs = 0;
        b->bytes = 0;
    }
    if (q->max_msgs > 0 && b->msgs >= q->max_msgs) return 0;
    if (q->max_bytes > 0 && b->bytes + msg_len > q->max_bytes) return 0;
    b->msgs++;
    b->bytes += msg_len;
    return 1;
}

int cmq_quota_check_connect(cmq_quota_t *q, const char *account) {
    if (!q || !account) return 1;
    if (q->max_connects == 0) return 1;
    struct cmq_account_bucket *b = find_or_create(q, account);
    if (!b) return 1;
    uint64_t now = now_ms();
    if (now - b->window_start_ms >= 1000) {
        b->window_start_ms = now;
        b->connects = 0;
    }
    if (b->connects >= q->max_connects) return 0;
    b->connects++;
    return 1;
}
