#define _POSIX_C_SOURCE 200809L
#include "cmq_queue.h"
#include <stdlib.h>
#include <time.h>

/* Lock-free MPSC queue (multiple producers, single consumer).
   Consumer must be a single thread — head is not CAS-protected. */

void cmq_queue_init(cmq_queue_t *q) {
    q->stub = malloc(sizeof(cmq_queue_node_t));
    if (!q->stub) {
        q->head = q->tail = NULL;
        atomic_init(&q->in_flight, 0);
        atomic_init(&q->dying, 0);
        return;
    }
    q->stub->next = NULL;
    q->stub->data = NULL;
    q->head = q->stub;
    q->tail = q->stub;
    atomic_init(&q->in_flight, 0);
    atomic_init(&q->dying, 0);
}

/* Drain helper — no dying/in_flight; destroy waits for ops then calls this. */
static void *queue_pop_impl(cmq_queue_t *q) {
    if (!q || !q->head) return NULL;
    cmq_queue_node_t *head = q->head;
    cmq_queue_node_t *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    if (next == NULL) return NULL;
    void *data = next->data;
    q->head = next;
    free(head);
    return data;
}

void cmq_queue_destroy(cmq_queue_t *q) {
    if (!q) return;
    atomic_store_explicit(&q->dying, 1, memory_order_release);
    while (atomic_load_explicit(&q->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    while (queue_pop_impl(q) != NULL) {}
    /* After drain, head is the remaining sentinel (original stub may already
       have been freed by the first successful pop). */
    free(q->head);
    q->stub = NULL;
    q->head = NULL;
    q->tail = NULL;
}

int cmq_queue_push(cmq_queue_t *q, void *data) {
    if (!q || !q->tail) return -1;
    if (atomic_load_explicit(&q->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&q->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&q->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&q->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    cmq_queue_node_t *node = malloc(sizeof(cmq_queue_node_t));
    if (!node) {
        atomic_fetch_sub_explicit(&q->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    node->data = data;
    node->next = NULL;

    cmq_queue_node_t *prev = __atomic_exchange_n(&q->tail, node, __ATOMIC_RELEASE);
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);
    atomic_fetch_sub_explicit(&q->in_flight, 1, memory_order_acq_rel);
    return 0;
}

void *cmq_queue_pop(cmq_queue_t *q) {
    if (!q || !q->head) return NULL;
    if (atomic_load_explicit(&q->dying, memory_order_acquire))
        return NULL;
    atomic_fetch_add_explicit(&q->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&q->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&q->in_flight, 1, memory_order_acq_rel);
        return NULL;
    }
    void *data = queue_pop_impl(q);
    atomic_fetch_sub_explicit(&q->in_flight, 1, memory_order_acq_rel);
    return data;
}
