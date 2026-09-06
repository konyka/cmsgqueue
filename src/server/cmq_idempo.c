#define _POSIX_C_SOURCE 200809L
#include "cmq_idempo.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t pid;
    uint8_t used;
    uint64_t last;
    uint64_t bits;
} cmq_idempo_slot_t;

struct cmq_idempo {
    cmq_idempo_slot_t slots[CMQ_IDEMPO_PIDS];
    cmq_mutex_t lock;
};

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t)get_be32(p) << 32) | (uint64_t)get_be32(p + 4);
}

cmq_idempo_t *cmq_idempo_create(void) {
    cmq_idempo_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    cmq_mutex_init(&t->lock);
    return t;
}

void cmq_idempo_destroy(cmq_idempo_t *t) {
    if (!t) return;
    cmq_mutex_destroy(&t->lock);
    free(t);
}

int cmq_idempo_encode(uint8_t *out, size_t cap, uint32_t pid, uint64_t seq,
                      size_t *out_len) {
    if (!out || cap < CMQ_IDEMPO_HDR_LEN || !out_len || pid == 0)
        return -1;
    memcpy(out, CMQ_IDEMPO_MAGIC, 4);
    put_be32(out + 4, pid);
    put_be64(out + 8, seq);
    *out_len = CMQ_IDEMPO_HDR_LEN;
    return 0;
}

int cmq_idempo_parse(const uint8_t *hdr, size_t n, uint32_t *pid,
                     uint64_t *seq) {
    if (!hdr || n < CMQ_IDEMPO_HDR_LEN || !pid || !seq) return -1;
    if (memcmp(hdr, CMQ_IDEMPO_MAGIC, 4) != 0) return -1;
    uint32_t p = get_be32(hdr + 4);
    if (p == 0) return -1;
    *pid = p;
    *seq = get_be64(hdr + 8);
    return 0;
}

static int idempo_find(cmq_idempo_t *t, uint32_t pid, int alloc) {
    uint32_t h = pid * 2654435761u;
    int empty = -1;
    for (int i = 0; i < CMQ_IDEMPO_PIDS; i++) {
        uint32_t idx = (h + (uint32_t)i) % (uint32_t)CMQ_IDEMPO_PIDS;
        if (t->slots[idx].used && t->slots[idx].pid == pid)
            return (int)idx;
        if (!t->slots[idx].used && empty < 0)
            empty = (int)idx;
    }
    if (alloc && empty >= 0) return empty;
    return alloc ? -2 : -1;
}

int cmq_idempo_check(cmq_idempo_t *t, uint32_t pid, uint64_t seq) {
    if (!t || pid == 0) return -1;
    cmq_mutex_lock(&t->lock);
    int idx = idempo_find(t, pid, 1);
    if (idx == -2) {
        cmq_mutex_unlock(&t->lock);
        return -2;
    }
    cmq_idempo_slot_t *s = &t->slots[idx];
    if (!s->used) {
        s->used = 1;
        s->pid = pid;
        s->last = seq;
        s->bits = 1ull;
        cmq_mutex_unlock(&t->lock);
        return 1;
    }
    if (seq > s->last) {
        uint64_t d = seq - s->last;
        if (d >= (uint64_t)CMQ_IDEMPO_WIN)
            s->bits = 1ull;
        else
            s->bits = (s->bits << d) | 1ull;
        s->last = seq;
        cmq_mutex_unlock(&t->lock);
        return 1;
    }
    uint64_t d = s->last - seq;
    if (d >= (uint64_t)CMQ_IDEMPO_WIN) {
        cmq_mutex_unlock(&t->lock);
        return 0;
    }
    uint64_t mask = 1ull << d;
    if (s->bits & mask) {
        cmq_mutex_unlock(&t->lock);
        return 0;
    }
    s->bits |= mask;
    cmq_mutex_unlock(&t->lock);
    return 1;
}
