#ifndef CMQ_LEAF_H
#define CMQ_LEAF_H

#include "cmq_cluster.h"
#include <stdint.h>
#include <stddef.h>

#define CMQ_LEAF_MAX_CONNECTIONS 64

typedef struct cmq_leaf_node cmq_leaf_node_t;

typedef struct {
    char leaf_id[CMQ_NODE_ID_SIZE];
    char addr[CMQ_NODE_ADDR_SIZE];
    int port;
    int fd;
    int connected;
    uint64_t last_activity_ms;
    size_t subscriptions;
} cmq_leaf_conn_t;

cmq_leaf_node_t *cmq_leaf_create(const char *hub_addr, int hub_port);
void cmq_leaf_destroy(cmq_leaf_node_t *leaf);

const char *cmq_leaf_hub_addr(cmq_leaf_node_t *leaf);
int cmq_leaf_hub_port(cmq_leaf_node_t *leaf);

int cmq_leaf_connect(cmq_leaf_node_t *leaf);
int cmq_leaf_disconnect(cmq_leaf_node_t *leaf);
int cmq_leaf_is_connected(cmq_leaf_node_t *leaf);

int cmq_leaf_subscribe(cmq_leaf_node_t *leaf, const char *subject);
int cmq_leaf_unsubscribe(cmq_leaf_node_t *leaf, const char *subject);
size_t cmq_leaf_sub_count(cmq_leaf_node_t *leaf);

size_t cmq_leaf_accept_count(cmq_leaf_node_t *leaf);
int cmq_leaf_accept(cmq_leaf_node_t *leaf, int fd, const char *leaf_id);
int cmq_leaf_remove(cmq_leaf_node_t *leaf, const char *leaf_id);

#endif
