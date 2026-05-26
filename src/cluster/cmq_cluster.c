#define _POSIX_C_SOURCE 200809L
#include "cmq_cluster.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cmq_cluster {
    char name[64];
    char self_id[CMQ_NODE_ID_SIZE];
    cmq_node_info_t nodes[CMQ_CLUSTER_MAX_NODES];
    size_t count;
    cmq_mutex_t lock;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

cmq_cluster_t *cmq_cluster_create(const char *cluster_name, const char *self_id) {
    if (!cluster_name || !self_id) return NULL;
    cmq_cluster_t *c = calloc(1, sizeof(cmq_cluster_t));
    if (!c) return NULL;
    strncpy(c->name, cluster_name, sizeof(c->name) - 1);
    strncpy(c->self_id, self_id, CMQ_NODE_ID_SIZE - 1);
    c->count = 0;
    cmq_mutex_init(&c->lock);
    return c;
}

void cmq_cluster_destroy(cmq_cluster_t *cluster) {
    if (!cluster) return;
    cmq_mutex_destroy(&cluster->lock);
    free(cluster);
}

const char *cmq_cluster_name(cmq_cluster_t *cluster) {
    return cluster ? cluster->name : NULL;
}

const char *cmq_cluster_self_id(cmq_cluster_t *cluster) {
    return cluster ? cluster->self_id : NULL;
}

int cmq_cluster_add_node(cmq_cluster_t *cluster, const char *id,
                          const char *addr, int port) {
    if (!cluster || !id || !addr) return -1;
    cmq_mutex_lock(&cluster->lock);

    if (cluster->count >= CMQ_CLUSTER_MAX_NODES) {
        cmq_mutex_unlock(&cluster->lock);
        return -1;
    }

    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            strncpy(cluster->nodes[i].addr, addr, CMQ_NODE_ADDR_SIZE - 1);
            cluster->nodes[i].port = port;
            cluster->nodes[i].last_heartbeat_ms = now_ms();
            cmq_mutex_unlock(&cluster->lock);
            return 0;
        }
    }

    cmq_node_info_t *n = &cluster->nodes[cluster->count];
    strncpy(n->id, id, CMQ_NODE_ID_SIZE - 1);
    strncpy(n->addr, addr, CMQ_NODE_ADDR_SIZE - 1);
    n->port = port;
    n->state = CMQ_NODE_JOINING;
    n->last_heartbeat_ms = now_ms();
    n->connect_time_ms = now_ms();
    cluster->count++;

    cmq_mutex_unlock(&cluster->lock);
    return 0;
}

int cmq_cluster_remove_node(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return -1;
    cmq_mutex_lock(&cluster->lock);
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            memmove(&cluster->nodes[i], &cluster->nodes[i + 1],
                    (cluster->count - i - 1) * sizeof(cmq_node_info_t));
            cluster->count--;
            cmq_mutex_unlock(&cluster->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
    return -1;
}

cmq_node_info_t *cmq_cluster_get_node(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return NULL;
    cmq_mutex_lock(&cluster->lock);
    cmq_node_info_t *found = NULL;
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            found = &cluster->nodes[i];
            break;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
    return found;
}

int cmq_cluster_set_node_state(cmq_cluster_t *cluster, const char *id,
                                cmq_node_state_t state) {
    if (!cluster || !id) return -1;
    cmq_mutex_lock(&cluster->lock);
    int rc = -1;
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            cluster->nodes[i].state = state;
            rc = 0;
            break;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
    return rc;
}

void cmq_cluster_heartbeat(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return;
    cmq_mutex_lock(&cluster->lock);
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            cluster->nodes[i].last_heartbeat_ms = now_ms();
            break;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
}

size_t cmq_cluster_node_count(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    cmq_mutex_lock(&cluster->lock);
    size_t c = cluster->count;
    cmq_mutex_unlock(&cluster->lock);
    return c;
}

size_t cmq_cluster_active_count(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    cmq_mutex_lock(&cluster->lock);
    size_t active = 0;
    for (size_t i = 0; i < cluster->count; i++) {
        if (cluster->nodes[i].state == CMQ_NODE_ACTIVE) active++;
    }
    cmq_mutex_unlock(&cluster->lock);
    return active;
}

void cmq_cluster_list_nodes(cmq_cluster_t *cluster, cmq_node_info_t *out,
                             size_t max) {
    if (!cluster || !out) return;
    cmq_mutex_lock(&cluster->lock);
    size_t n = cluster->count < max ? cluster->count : max;
    memcpy(out, cluster->nodes, n * sizeof(cmq_node_info_t));
    cmq_mutex_unlock(&cluster->lock);
}

int64_t cmq_cluster_ms_since_heartbeat(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return -1;
    cmq_mutex_lock(&cluster->lock);
    int64_t diff = -1;
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            diff = (int64_t)(now_ms() - cluster->nodes[i].last_heartbeat_ms);
            break;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
    return diff;
}
