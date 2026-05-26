#ifndef CMQ_CLUSTER_H
#define CMQ_CLUSTER_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_CLUSTER_MAX_NODES 32
#define CMQ_NODE_ID_SIZE 16
#define CMQ_NODE_ADDR_SIZE 64

typedef enum {
    CMQ_NODE_UNKNOWN = 0,
    CMQ_NODE_JOINING,
    CMQ_NODE_ACTIVE,
    CMQ_NODE_LEAVING,
    CMQ_NODE_OFFLINE
} cmq_node_state_t;

typedef struct {
    char id[CMQ_NODE_ID_SIZE];
    char addr[CMQ_NODE_ADDR_SIZE];
    int port;
    cmq_node_state_t state;
    uint64_t last_heartbeat_ms;
    uint64_t connect_time_ms;
} cmq_node_info_t;

typedef struct cmq_cluster cmq_cluster_t;

cmq_cluster_t *cmq_cluster_create(const char *cluster_name, const char *self_id);
void cmq_cluster_destroy(cmq_cluster_t *cluster);

const char *cmq_cluster_name(cmq_cluster_t *cluster);
const char *cmq_cluster_self_id(cmq_cluster_t *cluster);

int cmq_cluster_add_node(cmq_cluster_t *cluster, const char *id,
                          const char *addr, int port);
int cmq_cluster_remove_node(cmq_cluster_t *cluster, const char *id);
cmq_node_info_t *cmq_cluster_get_node(cmq_cluster_t *cluster, const char *id);

int cmq_cluster_set_node_state(cmq_cluster_t *cluster, const char *id,
                                cmq_node_state_t state);
void cmq_cluster_heartbeat(cmq_cluster_t *cluster, const char *id);

size_t cmq_cluster_node_count(cmq_cluster_t *cluster);
size_t cmq_cluster_active_count(cmq_cluster_t *cluster);
void cmq_cluster_list_nodes(cmq_cluster_t *cluster, cmq_node_info_t *out, size_t max);

int64_t cmq_cluster_ms_since_heartbeat(cmq_cluster_t *cluster, const char *id);

#endif
