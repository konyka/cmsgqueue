#define _POSIX_C_SOURCE 200809L
#include "cmq_gateway.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

struct cmq_gateway {
    char local_cluster[64];
    cmq_gw_conn_t conns[CMQ_GW_MAX_CONNECTIONS];
    size_t conn_count;
    cmq_gw_cluster_info_t clusters[CMQ_GW_MAX_CLUSTERS];
    size_t cluster_count;
    cmq_mutex_t lock;
};

cmq_gateway_t *cmq_gateway_create(const char *local_cluster) {
    if (!local_cluster) return NULL;
    cmq_gateway_t *gw = calloc(1, sizeof(cmq_gateway_t));
    if (!gw) return NULL;
    strncpy(gw->local_cluster, local_cluster, sizeof(gw->local_cluster) - 1);
    cmq_mutex_init(&gw->lock);
    return gw;
}

void cmq_gateway_destroy(cmq_gateway_t *gw) {
    if (!gw) return;
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (gw->conns[i].fd >= 0) close(gw->conns[i].fd);
    }
    cmq_mutex_destroy(&gw->lock);
    free(gw);
}

int cmq_gateway_add_remote(cmq_gateway_t *gw, const char *cluster_name,
                            const char *addr, int port) {
    if (!gw || !cluster_name || !addr) return -1;
    cmq_mutex_lock(&gw->lock);

    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, cluster_name) == 0) {
            strncpy(gw->clusters[i].addr, addr, CMQ_NODE_ADDR_SIZE - 1);
            gw->clusters[i].port = port;
            gw->clusters[i].known = 1;
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }

    if (gw->cluster_count >= CMQ_GW_MAX_CLUSTERS) {
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    cmq_gw_cluster_info_t *ci = &gw->clusters[gw->cluster_count++];
    strncpy(ci->name, cluster_name, sizeof(ci->name) - 1);
    strncpy(ci->addr, addr, CMQ_NODE_ADDR_SIZE - 1);
    ci->port = port;
    ci->known = 1;

    cmq_mutex_unlock(&gw->lock);
    return 0;
}

int cmq_gateway_connect_remote(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw || !cluster_name) return -1;
    cmq_mutex_lock(&gw->lock);

    const char *addr = NULL;
    int port = 0;
    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, cluster_name) == 0) {
            addr = gw->clusters[i].addr;
            port = gw->clusters[i].port;
            break;
        }
    }
    if (!addr) {
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    for (size_t i = 0; i < gw->conn_count; i++) {
        if (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0) {
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }

    if (gw->conn_count >= CMQ_GW_MAX_CONNECTIONS) {
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, addr, &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    cmq_gw_conn_t *c = &gw->conns[gw->conn_count++];
    strncpy(c->remote_cluster, cluster_name, sizeof(c->remote_cluster) - 1);
    strncpy(c->remote_addr, addr, CMQ_NODE_ADDR_SIZE - 1);
    c->remote_port = port;
    c->fd = fd;
    c->connected = 1;

    cmq_mutex_unlock(&gw->lock);
    return 0;
}

int cmq_gateway_disconnect(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw || !cluster_name) return -1;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0) {
            close(gw->conns[i].fd);
            memmove(&gw->conns[i], &gw->conns[i + 1],
                    (gw->conn_count - i - 1) * sizeof(cmq_gw_conn_t));
            gw->conn_count--;
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return -1;
}

size_t cmq_gateway_forward(cmq_gateway_t *gw, const char *target_cluster,
                            const uint8_t *data, size_t len) {
    if (!gw || !data || len == 0) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t sent = 0;
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (strcmp(gw->conns[i].remote_cluster, target_cluster) == 0 &&
            gw->conns[i].connected) {
            ssize_t n = write(gw->conns[i].fd, data, len);
            if (n > 0) sent++;
            else if (n < 0 && errno != EAGAIN) gw->conns[i].connected = 0;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return sent;
}

size_t cmq_gateway_broadcast(cmq_gateway_t *gw, const uint8_t *data, size_t len) {
    if (!gw || !data || len == 0) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t sent = 0;
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (gw->conns[i].connected) {
            ssize_t n = write(gw->conns[i].fd, data, len);
            if (n > 0) sent++;
            else if (n < 0 && errno != EAGAIN) gw->conns[i].connected = 0;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return sent;
}

size_t cmq_gateway_connection_count(cmq_gateway_t *gw) {
    if (!gw) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t c = gw->conn_count;
    cmq_mutex_unlock(&gw->lock);
    return c;
}

size_t cmq_gateway_known_cluster_count(cmq_gateway_t *gw) {
    if (!gw) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t c = gw->cluster_count;
    cmq_mutex_unlock(&gw->lock);
    return c;
}

cmq_gw_cluster_info_t *cmq_gateway_get_cluster(cmq_gateway_t *gw,
                                                  const char *name) {
    if (!gw || !name) return NULL;
    cmq_mutex_lock(&gw->lock);
    cmq_gw_cluster_info_t *found = NULL;
    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, name) == 0) {
            found = &gw->clusters[i];
            break;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return found;
}
