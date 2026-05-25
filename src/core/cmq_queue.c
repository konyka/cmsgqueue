#include "cmq_queue.h"
#include <stdlib.h>

void cmq_queue_init(cmq_queue_t *q) {
    q->stub = malloc(sizeof(cmq_queue_node_t));
    q->stub->next = NULL;
    q->stub->data = NULL;
    q->head = q->stub;
    q->tail = q->stub;
}

void cmq_queue_destroy(cmq_queue_t *q) {
    while (cmq_queue_pop(q) != NULL) {}
    free(q->stub);
    q->stub = NULL;
    q->head = NULL;
    q->tail = NULL;
}

void cmq_queue_push(cmq_queue_t *q, void *data) {
    cmq_queue_node_t *node = malloc(sizeof(cmq_queue_node_t));
    node->data = data;
    node->next = NULL;

    cmq_queue_node_t *prev = __atomic_exchange_n(&q->tail, node, __ATOMIC_RELEASE);
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);
}

void *cmq_queue_pop(cmq_queue_t *q) {
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
