#include "cmq_queue.h"
#include <stdlib.h>

/* Lock-free MPSC queue (multiple producers, single consumer).
   Consumer must be a single thread — head is not CAS-protected. */

void cmq_queue_init(cmq_queue_t *q) {
    q->stub = malloc(sizeof(cmq_queue_node_t));
    if (!q->stub) {
        q->head = q->tail = NULL;
        return;
    }
    q->stub->next = NULL;
    q->stub->data = NULL;
    q->head = q->stub;
    q->tail = q->stub;
}

void cmq_queue_destroy(cmq_queue_t *q) {
    if (!q) return;
    while (cmq_queue_pop(q) != NULL) {}
    /* After drain, head is the remaining sentinel (original stub may already
       have been freed by the first successful pop). */
    free(q->head);
    q->stub = NULL;
    q->head = NULL;
    q->tail = NULL;
}

int cmq_queue_push(cmq_queue_t *q, void *data) {
    if (!q || !q->tail) return -1;
    cmq_queue_node_t *node = malloc(sizeof(cmq_queue_node_t));
    if (!node) return -1;
    node->data = data;
    node->next = NULL;

    cmq_queue_node_t *prev = __atomic_exchange_n(&q->tail, node, __ATOMIC_RELEASE);
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);
    return 0;
}

void *cmq_queue_pop(cmq_queue_t *q) {
    if (!q || !q->head) return NULL;
    cmq_queue_node_t *head = q->head;
    cmq_queue_node_t *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);

    if (next == NULL) {
        return NULL;
    }

    void *data = next->data;
    q->head = next;
    free(head);

    return data;
}
