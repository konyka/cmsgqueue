#ifndef CMQ_GATEWAY_H
#define CMQ_GATEWAY_H

#include "cmq_cluster.h"
#include <stdint.h>
#include <stddef.h>

#define CMQ_GW_MAX_CONNECTIONS 16
#define CMQ_GW_MAX_CLUSTERS 8

typedef struct cmq_gateway cmq_gateway_t;

typedef struct {
    char remote_cluster[64];
    char remote_addr[CMQ_NODE_ADDR_SIZE];
    int remote_port;
    int fd;
    int connected;
    uint64_t last_ping_ms;
} cmq_gw_conn_t;

typedef struct {
    char name[64];
    char addr[CMQ_NODE_ADDR_SIZE];
    int port;
    int known;
} cmq_gw_cluster_info_t;

cmq_gateway_t *cmq_gateway_create(const char *local_cluster);
void cmq_gateway_destroy(cmq_gateway_t *gw);

int cmq_gateway_add_remote(cmq_gateway_t *gw, const char *cluster_name,
                            const char *addr, int port);
int cmq_gateway_connect_remote(cmq_gateway_t *gw, const char *cluster_name);
int cmq_gateway_disconnect(cmq_gateway_t *gw, const char *cluster_name);

size_t cmq_gateway_forward(cmq_gateway_t *gw, const char *target_cluster,
                            const uint8_t *data, size_t len);
size_t cmq_gateway_broadcast(cmq_gateway_t *gw, const uint8_t *data, size_t len);

size_t cmq_gateway_connection_count(cmq_gateway_t *gw);
size_t cmq_gateway_known_cluster_count(cmq_gateway_t *gw);
cmq_gw_cluster_info_t *cmq_gateway_get_cluster(cmq_gateway_t *gw,
                                                  const char *name);

#endif
