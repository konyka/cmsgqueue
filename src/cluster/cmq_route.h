#ifndef CMQ_ROUTE_H
#define CMQ_ROUTE_H

#include "cmq_cluster.h"
#include <stdint.h>
#include <stddef.h>

typedef struct cmq_route_pool cmq_route_pool_t;

typedef struct {
    char remote_id[CMQ_NODE_ID_SIZE];
    int fd;
    int connected;
    uint64_t msgs_sent;
    uint64_t msgs_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} cmq_route_conn_t;

cmq_route_pool_t *cmq_route_pool_create(cmq_cluster_t *cluster);
void cmq_route_pool_destroy(cmq_route_pool_t *pool);

int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port);
int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd);
int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id);

int cmq_route_forward(cmq_route_pool_t *pool, const char *subject,
                       const uint8_t *data, size_t len,
                       const char *exclude_id);
size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id);

size_t cmq_route_pool_count(cmq_route_pool_t *pool);
cmq_route_conn_t *cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id);

#endif
