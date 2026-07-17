#define _POSIX_C_SOURCE 200809L
#include "cmq_cluster.h"
#include "cmq_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

struct cmq_cluster {
    char name[64];
    char self_id[CMQ_NODE_ID_SIZE];
    cmq_node_info_t nodes[CMQ_CLUSTER_MAX_NODES];
    size_t count;
    cmq_mutex_t lock;
    atomic_int in_flight;
    atomic_int dying;
};

static int cluster_begin_op(cmq_cluster_t *cluster) {
    if (atomic_load_explicit(&cluster->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&cluster->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&cluster->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&cluster->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void cluster_end_op(cmq_cluster_t *cluster) {
    atomic_fetch_sub_explicit(&cluster->in_flight, 1, memory_order_acq_rel);
}

static int cluster_id_ok(const char *id) {
    if (!id || !id[0]) return 0;
    size_t n = strnlen(id, CMQ_NODE_ID_SIZE);
    return n > 0 && n < CMQ_NODE_ID_SIZE;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

cmq_cluster_t *cmq_cluster_create(const char *cluster_name, const char *self_id) {
    if (!cluster_name || !self_id) return NULL;
    /* name[64] / self_id[CMQ_NODE_ID_SIZE] — reject before truncate-collide. */
    if (strnlen(cluster_name, 64) >= 64 ||
        strnlen(self_id, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE)
        return NULL;
    cmq_cluster_t *c = calloc(1, sizeof(cmq_cluster_t));
    if (!c) return NULL;
    snprintf(c->name, sizeof(c->name), "%s", cluster_name);
    snprintf(c->self_id, sizeof(c->self_id), "%s", self_id);
    c->count = 0;
    atomic_init(&c->in_flight, 0);
    atomic_init(&c->dying, 0);
    cmq_mutex_init(&c->lock);
    return c;
}

void cmq_cluster_destroy(cmq_cluster_t *cluster) {
    if (!cluster) return;
    atomic_store_explicit(&cluster->dying, 1, memory_order_release);
    while (atomic_load_explicit(&cluster->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    cmq_mutex_destroy(&cluster->lock);
    free(cluster);
}

const char *cmq_cluster_name(cmq_cluster_t *cluster) {
    if (!cluster || atomic_load_explicit(&cluster->dying, memory_order_acquire))
        return NULL;
    return cluster->name;
}

const char *cmq_cluster_self_id(cmq_cluster_t *cluster) {
    if (!cluster || atomic_load_explicit(&cluster->dying, memory_order_acquire))
        return NULL;
    return cluster->self_id;
}

static int cluster_add_node_impl(cmq_cluster_t *cluster, const char *id,
                          const char *addr, int port) {
    if (!cluster || !cluster_id_ok(id) || !addr) return -1;
    if (strnlen(addr, CMQ_NODE_ADDR_SIZE) >= CMQ_NODE_ADDR_SIZE)
        return -1;
    cmq_mutex_lock(&cluster->lock);

    if (cluster->count >= CMQ_CLUSTER_MAX_NODES) {
        cmq_mutex_unlock(&cluster->lock);
        return -1;
    }

    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            /* snprintf clears leftover bytes when the new addr is shorter. */
            snprintf(cluster->nodes[i].addr, sizeof(cluster->nodes[i].addr),
                     "%s", addr);
            cluster->nodes[i].port = port;
            cluster->nodes[i].last_heartbeat_ms = now_ms();
            cmq_mutex_unlock(&cluster->lock);
            return 0;
        }
    }

    cmq_node_info_t *n = &cluster->nodes[cluster->count];
    snprintf(n->id, sizeof(n->id), "%s", id);
    snprintf(n->addr, sizeof(n->addr), "%s", addr);
    n->port = port;
    n->state = CMQ_NODE_JOINING;
    n->last_heartbeat_ms = now_ms();
    n->connect_time_ms = now_ms();
    cluster->count++;

    cmq_mutex_unlock(&cluster->lock);
    return 0;
}

static int cluster_remove_node_impl(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !cluster_id_ok(id)) return -1;
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

static int cluster_get_node_impl(cmq_cluster_t *cluster, const char *id,
                          cmq_node_info_t *out) {
    if (!cluster || !cluster_id_ok(id) || !out) return -1;
    cmq_mutex_lock(&cluster->lock);
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            *out = cluster->nodes[i];
            cmq_mutex_unlock(&cluster->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
    return -1;
}

static int cluster_set_node_state_impl(cmq_cluster_t *cluster, const char *id,
                                cmq_node_state_t state) {
    if (!cluster || !cluster_id_ok(id)) return -1;
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

static void cluster_heartbeat_impl(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !cluster_id_ok(id)) return;
    cmq_mutex_lock(&cluster->lock);
    for (size_t i = 0; i < cluster->count; i++) {
        if (strcmp(cluster->nodes[i].id, id) == 0) {
            cluster->nodes[i].last_heartbeat_ms = now_ms();
            break;
        }
    }
    cmq_mutex_unlock(&cluster->lock);
}

static size_t cluster_node_count_impl(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    cmq_mutex_lock(&cluster->lock);
    size_t c = cluster->count;
    cmq_mutex_unlock(&cluster->lock);
    return c;
}

static size_t cluster_active_count_impl(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    cmq_mutex_lock(&cluster->lock);
    size_t active = 0;
    for (size_t i = 0; i < cluster->count; i++) {
        if (cluster->nodes[i].state == CMQ_NODE_ACTIVE) active++;
    }
    cmq_mutex_unlock(&cluster->lock);
    return active;
}

static void cluster_list_nodes_impl(cmq_cluster_t *cluster, cmq_node_info_t *out,
                             size_t max) {
    if (!cluster || !out) return;
    cmq_mutex_lock(&cluster->lock);
    size_t n = cluster->count < max ? cluster->count : max;
    memcpy(out, cluster->nodes, n * sizeof(cmq_node_info_t));
    cmq_mutex_unlock(&cluster->lock);
}

static int64_t cluster_ms_since_heartbeat_impl(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !cluster_id_ok(id)) return -1;
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

int cmq_cluster_add_node(cmq_cluster_t *cluster, const char *id,
                          const char *addr, int port) {
    if (!cluster || !id || !addr) return -1;
    if (cluster_begin_op(cluster) != 0) return -1;
    int rc = cluster_add_node_impl(cluster, id, addr, port);
    cluster_end_op(cluster);
    return rc;
}

int cmq_cluster_remove_node(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return -1;
    if (cluster_begin_op(cluster) != 0) return -1;
    int rc = cluster_remove_node_impl(cluster, id);
    cluster_end_op(cluster);
    return rc;
}

int cmq_cluster_get_node(cmq_cluster_t *cluster, const char *id,
                          cmq_node_info_t *out) {
    if (!cluster || !id || !out) return -1;
    if (cluster_begin_op(cluster) != 0) return -1;
    int rc = cluster_get_node_impl(cluster, id, out);
    cluster_end_op(cluster);
    return rc;
}

int cmq_cluster_set_node_state(cmq_cluster_t *cluster, const char *id,
                                cmq_node_state_t state) {
    if (!cluster || !id) return -1;
    if (cluster_begin_op(cluster) != 0) return -1;
    int rc = cluster_set_node_state_impl(cluster, id, state);
    cluster_end_op(cluster);
    return rc;
}

void cmq_cluster_heartbeat(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return;
    if (cluster_begin_op(cluster) != 0) return;
    cluster_heartbeat_impl(cluster, id);
    cluster_end_op(cluster);
}

size_t cmq_cluster_node_count(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    if (cluster_begin_op(cluster) != 0) return 0;
    size_t c = cluster_node_count_impl(cluster);
    cluster_end_op(cluster);
    return c;
}

size_t cmq_cluster_active_count(cmq_cluster_t *cluster) {
    if (!cluster) return 0;
    if (cluster_begin_op(cluster) != 0) return 0;
    size_t c = cluster_active_count_impl(cluster);
    cluster_end_op(cluster);
    return c;
}

void cmq_cluster_list_nodes(cmq_cluster_t *cluster, cmq_node_info_t *out,
                             size_t max) {
    if (!cluster || !out) return;
    if (cluster_begin_op(cluster) != 0) return;
    cluster_list_nodes_impl(cluster, out, max);
    cluster_end_op(cluster);
}

int64_t cmq_cluster_ms_since_heartbeat(cmq_cluster_t *cluster, const char *id) {
    if (!cluster || !id) return -1;
    if (cluster_begin_op(cluster) != 0) return -1;
    int64_t d = cluster_ms_since_heartbeat_impl(cluster, id);
    cluster_end_op(cluster);
    return d;
}

