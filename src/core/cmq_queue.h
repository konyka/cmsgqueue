#ifndef CMQ_QUEUE_H
#define CMQ_QUEUE_H

#include "cmq_atomic.h"
#include "cmq_platform.h"

typedef struct cmq_queue_node {
    void *data;
    struct cmq_queue_node *next;
} cmq_queue_node_t;

typedef struct {
    cmq_queue_node_t *head;
    cmq_queue_node_t *tail;
    cmq_queue_node_t *stub;
} cmq_queue_t;

void cmq_queue_init(cmq_queue_t *q);
void cmq_queue_destroy(cmq_queue_t *q);
void cmq_queue_push(cmq_queue_t *q, void *data);
void *cmq_queue_pop(cmq_queue_t *q);

#endif
