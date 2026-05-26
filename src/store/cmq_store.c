#define _POSIX_C_SOURCE 200809L
#include "cmq_store.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct cmq_store_slot {
    uint64_t seq;
    uint8_t *data;
    size_t len;
    uint64_t timestamp_ms;
    int valid;
} cmq_store_slot_t;

struct cmq_store {
    cmq_store_slot_t *ring;
    size_t cap;
    uint64_t head_seq;
    size_t count;
    cmq_mutex_t lock;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

cmq_store_t *cmq_store_create(size_t capacity) {
    if (capacity == 0) capacity = 1024;
    cmq_store_t *s = calloc(1, sizeof(cmq_store_t));
    if (!s) return NULL;
    s->ring = calloc(capacity, sizeof(cmq_store_slot_t));
    if (!s->ring) { free(s); return NULL; }
    s->cap = capacity;
    s->head_seq = 1;
    s->count = 0;
    cmq_mutex_init(&s->lock);
    return s;
}

void cmq_store_destroy(cmq_store_t *store) {
    if (!store) return;
    for (size_t i = 0; i < store->cap; i++) {
        free(store->ring[i].data);
    }
    free(store->ring);
    cmq_mutex_destroy(&store->lock);
    free(store);
}

uint64_t cmq_store_put(cmq_store_t *store, const uint8_t *data, size_t len) {
    if (!store || !data || len == 0) return 0;
    cmq_mutex_lock(&store->lock);

    size_t idx = (size_t)(store->head_seq - 1) % store->cap;
    free(store->ring[idx].data);

    store->ring[idx].data = malloc(len);
    if (!store->ring[idx].data) {
        cmq_mutex_unlock(&store->lock);
        return 0;
    }
    memcpy(store->ring[idx].data, data, len);
    store->ring[idx].seq = store->head_seq;
    store->ring[idx].len = len;
    store->ring[idx].timestamp_ms = now_ms();
    store->ring[idx].valid = 1;

    uint64_t assigned = store->head_seq;
    store->head_seq++;

    if (store->count < store->cap) {
        store->count++;
    }

    cmq_mutex_unlock(&store->lock);
    return assigned;
}

int cmq_store_get(cmq_store_t *store, uint64_t seq, cmq_store_msg_t *out) {
    if (!store || !out || seq == 0) return -1;
    cmq_mutex_lock(&store->lock);

    if (seq >= store->head_seq) {
        cmq_mutex_unlock(&store->lock);
        return -1;
    }

    uint64_t oldest = (store->head_seq > store->cap) ? store->head_seq - store->cap : 1;
    if (seq < oldest) {
        cmq_mutex_unlock(&store->lock);
        return -1;
    }

    size_t idx = (size_t)(seq - 1) % store->cap;
    cmq_store_slot_t *slot = &store->ring[idx];

    if (!slot->valid || slot->seq != seq) {
        cmq_mutex_unlock(&store->lock);
        return -1;
    }

    out->seq = slot->seq;
    out->data = slot->data;
    out->len = slot->len;
    out->timestamp_ms = slot->timestamp_ms;

    cmq_mutex_unlock(&store->lock);
    return 0;
}

size_t cmq_store_count(cmq_store_t *store) {
    if (!store) return 0;
    cmq_mutex_lock(&store->lock);
    size_t c = store->count;
    cmq_mutex_unlock(&store->lock);
    return c;
}

uint64_t cmq_store_first_seq(cmq_store_t *store) {
    if (!store) return 0;
    cmq_mutex_lock(&store->lock);
    uint64_t first = (store->head_seq > store->count) ? store->head_seq - store->count : 1;
    cmq_mutex_unlock(&store->lock);
    return first;
}

uint64_t cmq_store_last_seq(cmq_store_t *store) {
    if (!store) return 0;
    cmq_mutex_lock(&store->lock);
    uint64_t last = store->head_seq - 1;
    cmq_mutex_unlock(&store->lock);
    return last;
}

void cmq_store_truncate(cmq_store_t *store, uint64_t before_seq) {
    if (!store) return;
    cmq_mutex_lock(&store->lock);
    for (uint64_t seq = 1; seq < before_seq && seq < store->head_seq; seq++) {
        size_t idx = (size_t)(seq - 1) % store->cap;
        if (store->ring[idx].seq == seq && store->ring[idx].valid) {
            free(store->ring[idx].data);
            store->ring[idx].data = NULL;
            store->ring[idx].valid = 0;
            if (store->count > 0) store->count--;
        }
    }
    cmq_mutex_unlock(&store->lock);
}
