#define _POSIX_C_SOURCE 200809L
#include "cmq_route.h"
#include "cmq_thread.h"
#include "cmq_types.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define CMQ_ROUTE_MAX_CONNS 32

typedef struct {
    char subject[256];
    char dest_id[CMQ_NODE_ID_SIZE];
    int active;
} cmq_route_interest_t;

struct cmq_route_pool {
    cmq_cluster_t *cluster;
    cmq_route_conn_t conns[CMQ_ROUTE_MAX_CONNS];
    size_t conn_count;
    cmq_route_interest_t interests[256];
    size_t interest_count;
    cmq_mutex_t lock;
};

cmq_route_pool_t *cmq_route_pool_create(cmq_cluster_t *cluster) {
    cmq_route_pool_t *p = calloc(1, sizeof(cmq_route_pool_t));
    if (!p) return NULL;
    p->cluster = cluster;
    p->conn_count = 0;
    p->interest_count = 0;
    cmq_mutex_init(&p->lock);
    return p;
}

void cmq_route_pool_destroy(cmq_route_pool_t *pool) {
    if (!pool) return;
    for (size_t i = 0; i < pool->conn_count; i++) {
        if (pool->conns[i].fd >= 0) close(pool->conns[i].fd);
    }
    cmq_mutex_destroy(&pool->lock);
    free(pool);
}

int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port) {
    if (!pool || !node_id || !addr) return -1;
    cmq_mutex_lock(&pool->lock);

    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
    }

    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, addr, &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }

    cmq_route_conn_t *c = &pool->conns[pool->conn_count++];
    strncpy(c->remote_id, node_id, CMQ_NODE_ID_SIZE - 1);
    c->fd = fd;
    c->connected = 1;
    c->msgs_sent = 0;
    c->msgs_recv = 0;
    c->bytes_sent = 0;
    c->bytes_recv = 0;

    cmq_mutex_unlock(&pool->lock);
    return 0;
}

int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd) {
    if (!pool || !node_id) return -1;
    cmq_mutex_lock(&pool->lock);

    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
    }

    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }

    cmq_route_conn_t *c = &pool->conns[pool->conn_count++];
    strncpy(c->remote_id, node_id, CMQ_NODE_ID_SIZE - 1);
    c->fd = fd;
    c->connected = 1;
    c->msgs_sent = 0;
    c->msgs_recv = 0;
    c->bytes_sent = 0;
    c->bytes_recv = 0;

    cmq_mutex_unlock(&pool->lock);
    return 0;
}

int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            if (pool->conns[i].fd >= 0) close(pool->conns[i].fd);
            memmove(&pool->conns[i], &pool->conns[i + 1],
                    (pool->conn_count - i - 1) * sizeof(cmq_route_conn_t));
            pool->conn_count--;
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&pool->lock);
    return -1;
}

int cmq_route_forward(cmq_route_pool_t *pool, const char *subject __attribute__((unused)),
                       const uint8_t *data, size_t len,
                       const char *exclude_id) {
    if (!pool || !data || len == 0) return -1;
    cmq_mutex_lock(&pool->lock);
    int sent = 0;
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_route_conn_t *c = &pool->conns[i];
        if (!c->connected) continue;
        if (exclude_id && strcmp(c->remote_id, exclude_id) == 0) continue;
        ssize_t n = write(c->fd, data, len);
        if (n > 0) {
            c->bytes_sent += (uint64_t)n;
            c->msgs_sent++;
            sent++;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->connected = 0;
        }
    }
    cmq_mutex_unlock(&pool->lock);
    return sent > 0 ? 0 : -1;
}

size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id) {
    if (!pool || !data || len == 0) return 0;
    cmq_mutex_lock(&pool->lock);
    size_t sent = 0;
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_route_conn_t *c = &pool->conns[i];
        if (!c->connected) continue;
        if (exclude_id && strcmp(c->remote_id, exclude_id) == 0) continue;
        ssize_t n = write(c->fd, data, len);
        if (n > 0) {
            c->bytes_sent += (uint64_t)n;
            c->msgs_sent++;
            sent++;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->connected = 0;
        }
    }
    cmq_mutex_unlock(&pool->lock);
    return sent;
}

size_t cmq_route_pool_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    cmq_mutex_lock(&pool->lock);
    size_t c = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    return c;
}

cmq_route_conn_t *cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return NULL;
    cmq_mutex_lock(&pool->lock);
    cmq_route_conn_t *found = NULL;
    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            found = &pool->conns[i];
            break;
        }
    }
    cmq_mutex_unlock(&pool->lock);
    return found;
}
