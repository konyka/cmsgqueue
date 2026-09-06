#define _POSIX_C_SOURCE 200809L
#include "cmq_otel.h"
#include "cmq_thread.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    cmq_otel_span_t span;
    atomic_int ready;
} cmq_otel_slot_t;

struct cmq_otel {
    cmq_otel_slot_t slots[CMQ_OTEL_RING];
    atomic_uint_fast64_t wpos;
    atomic_uint_fast64_t rpos;
    atomic_uint_fast64_t dropped;
    atomic_uint_fast64_t exported;
    atomic_int run;
    cmq_otel_export_fn export_fn;
    void *export_ctx;
    cmq_thread_t thr;
    int started;
};

static uint64_t otel_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

cmq_otel_t *cmq_otel_create(void) {
    cmq_otel_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    atomic_init(&o->wpos, 0);
    atomic_init(&o->rpos, 0);
    atomic_init(&o->dropped, 0);
    atomic_init(&o->exported, 0);
    atomic_init(&o->run, 0);
    for (int i = 0; i < CMQ_OTEL_RING; i++)
        atomic_init(&o->slots[i].ready, 0);
    return o;
}

static void *otel_sidecar(void *arg) {
    cmq_otel_t *o = arg;
    cmq_otel_span_t s;
    while (atomic_load_explicit(&o->run, memory_order_acquire)) {
        if (cmq_otel_poll(o, &s) == 1) {
            if (o->export_fn)
                o->export_fn(o->export_ctx, &s);
            atomic_fetch_add_explicit(&o->exported, 1, memory_order_relaxed);
        } else {
            struct timespec ts = {0, 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    while (cmq_otel_poll(o, &s) == 1) {
        if (o->export_fn)
            o->export_fn(o->export_ctx, &s);
        atomic_fetch_add_explicit(&o->exported, 1, memory_order_relaxed);
    }
    return NULL;
}

void cmq_otel_destroy(cmq_otel_t *o) {
    if (!o) return;
    cmq_otel_stop(o);
    free(o);
}

int cmq_otel_offer(cmq_otel_t *o, const uint8_t trace[16], uint8_t kind) {
    if (!o || !trace || kind < CMQ_OTEL_KIND_PUBLISH ||
        kind > CMQ_OTEL_KIND_DISCONNECT)
        return -1;
    for (;;) {
        uint64_t w = atomic_load_explicit(&o->wpos, memory_order_relaxed);
        uint64_t r = atomic_load_explicit(&o->rpos, memory_order_acquire);
        if (w - r >= (uint64_t)CMQ_OTEL_RING) {
            atomic_fetch_add_explicit(&o->dropped, 1, memory_order_relaxed);
            return 1;
        }
        if (!atomic_compare_exchange_weak_explicit(&o->wpos, &w, w + 1,
                memory_order_acq_rel, memory_order_relaxed))
            continue;
        cmq_otel_slot_t *slot = &o->slots[w % (uint64_t)CMQ_OTEL_RING];
        memcpy(slot->span.trace, trace, 16);
        slot->span.kind = kind;
        slot->span.t_ms = otel_now_ms();
        atomic_store_explicit(&slot->ready, 1, memory_order_release);
        return 0;
    }
}

int cmq_otel_on_consume(cmq_otel_t *o, const uint8_t trace[16], int delivered) {
    if (!o || !trace) return -1;
    if (!delivered) return 0;
    return cmq_otel_offer(o, trace, CMQ_OTEL_KIND_CONSUME);
}

int cmq_otel_on_connect(cmq_otel_t *o, const uint8_t trace[16], int ok) {
    if (!o || !trace) return -1;
    if (!ok) return 0;
    return cmq_otel_offer(o, trace, CMQ_OTEL_KIND_CONNECT);
}

int cmq_otel_on_request(cmq_otel_t *o, const uint8_t trace[16], int answered) {
    if (!o || !trace) return -1;
    if (!answered) return 0;
    return cmq_otel_offer(o, trace, CMQ_OTEL_KIND_REQUEST);
}

int cmq_otel_on_response(cmq_otel_t *o, const uint8_t trace[16], int delivered) {
    if (!o || !trace) return -1;
    if (!delivered) return 0;
    return cmq_otel_offer(o, trace, CMQ_OTEL_KIND_RESPONSE);
}

int cmq_otel_on_disconnect(cmq_otel_t *o, const uint8_t trace[16], int ok) {
    if (!o || !trace) return -1;
    if (!ok) return 0;
    return cmq_otel_offer(o, trace, CMQ_OTEL_KIND_DISCONNECT);
}

int cmq_otel_poll(cmq_otel_t *o, cmq_otel_span_t *out) {
    if (!o || !out) return 0;
    uint64_t r = atomic_load_explicit(&o->rpos, memory_order_relaxed);
    cmq_otel_slot_t *slot = &o->slots[r % (uint64_t)CMQ_OTEL_RING];
    if (!atomic_load_explicit(&slot->ready, memory_order_acquire))
        return 0;
    *out = slot->span;
    atomic_store_explicit(&slot->ready, 0, memory_order_release);
    atomic_store_explicit(&o->rpos, r + 1, memory_order_release);
    return 1;
}

uint64_t cmq_otel_dropped(const cmq_otel_t *o) {
    return o ? atomic_load_explicit(&o->dropped, memory_order_relaxed) : 0;
}

uint64_t cmq_otel_exported(const cmq_otel_t *o) {
    return o ? atomic_load_explicit(&o->exported, memory_order_relaxed) : 0;
}

void cmq_otel_set_export(cmq_otel_t *o, cmq_otel_export_fn fn, void *ctx) {
    if (!o) return;
    o->export_fn = fn;
    o->export_ctx = ctx;
}

int cmq_otel_start(cmq_otel_t *o) {
    if (!o || o->started) return -1;
    atomic_store_explicit(&o->run, 1, memory_order_release);
    if (cmq_thread_create(&o->thr, otel_sidecar, o) != 0) {
        atomic_store_explicit(&o->run, 0, memory_order_release);
        return -1;
    }
    o->started = 1;
    return 0;
}

void cmq_otel_stop(cmq_otel_t *o) {
    if (!o || !o->started) return;
    atomic_store_explicit(&o->run, 0, memory_order_release);
    cmq_thread_join(o->thr);
    o->started = 0;
}
