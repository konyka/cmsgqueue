#define _POSIX_C_SOURCE 200809L
#include "cmq_gateway.h"
#include "cmq_route.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

struct cmq_gateway {
    char local_cluster[64];
    char auth_user[256];
    char auth_pass[256];
    cmq_gw_conn_t conns[CMQ_GW_MAX_CONNECTIONS];
    size_t conn_count;
    cmq_gw_cluster_info_t clusters[CMQ_GW_MAX_CLUSTERS];
    size_t cluster_count;
    cmq_mutex_t lock;
    cmq_mutex_t io_locks[CMQ_GW_MAX_CONNECTIONS]; /* per-slot write serialization */
};

#define CMQ_GW_WRITE_MS 50
#define CMQ_GW_CONNECT_MS 2000

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Close slot fd under io_lock so concurrent write_full cannot race close.
   Also clear identity fields under the same lock (forward/broadcast verify
   under io_lock only — never nest gw->lock). Caller must hold gw->lock.
   Lock order: gw->lock → io_lock. */
static void gw_slot_close_fd(cmq_gateway_t *gw, size_t idx) {
    if (idx >= CMQ_GW_MAX_CONNECTIONS) return;
    cmq_mutex_lock(&gw->io_locks[idx]);
    int fd = gw->conns[idx].fd;
    memset(&gw->conns[idx], 0, sizeof(gw->conns[idx]));
    gw->conns[idx].fd = -1;
    if (fd >= 0) close(fd);
    cmq_mutex_unlock(&gw->io_locks[idx]);
}

/* Publish a live peer into a slot. Caller holds gw->lock; takes io_lock. */
static void gw_slot_install(cmq_gateway_t *gw, size_t idx, const char *cluster,
                             const char *addr, int port, int fd) {
    cmq_mutex_lock(&gw->io_locks[idx]);
    memset(&gw->conns[idx], 0, sizeof(gw->conns[idx]));
    strncpy(gw->conns[idx].remote_cluster, cluster,
            sizeof(gw->conns[idx].remote_cluster) - 1);
    strncpy(gw->conns[idx].remote_addr, addr, CMQ_NODE_ADDR_SIZE - 1);
    gw->conns[idx].remote_port = port;
    gw->conns[idx].fd = fd;
    gw->conns[idx].connected = 1;
    cmq_mutex_unlock(&gw->io_locks[idx]);
}

/* 0 = full write, 1 = EAGAIN zero progress, -1 = hard failure. */
static int write_full(int fd, const uint8_t *data, size_t len) {
    size_t off = 0;
    int stall_rounds = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (off == 0) return 1;
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                for (;;) {
                    int pr = poll(&pfd, 1, CMQ_GW_WRITE_MS);
                    if (pr > 0) {
                        stall_rounds = 0;
                        break;
                    }
                    if (pr < 0 && errno == EINTR) continue;
                    if (pr == 0 && ++stall_rounds < 4)
                        continue;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

cmq_gateway_t *cmq_gateway_create(const char *local_cluster) {
    if (!local_cluster) return NULL;
    cmq_gateway_t *gw = calloc(1, sizeof(cmq_gateway_t));
    if (!gw) return NULL;
    strncpy(gw->local_cluster, local_cluster, sizeof(gw->local_cluster) - 1);
    cmq_mutex_init(&gw->lock);
    for (size_t i = 0; i < CMQ_GW_MAX_CONNECTIONS; i++)
        cmq_mutex_init(&gw->io_locks[i]);
    return gw;
}

void cmq_gateway_destroy(cmq_gateway_t *gw) {
    if (!gw) return;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count; i++)
        gw_slot_close_fd(gw, i);
    cmq_mutex_unlock(&gw->lock);
    for (size_t i = 0; i < CMQ_GW_MAX_CONNECTIONS; i++)
        cmq_mutex_destroy(&gw->io_locks[i]);
    cmq_mutex_destroy(&gw->lock);
    free(gw);
}

int cmq_gateway_set_auth(cmq_gateway_t *gw, const char *user, const char *pass) {
    if (!gw) return -1;
    cmq_mutex_lock(&gw->lock);
    memset(gw->auth_user, 0, sizeof(gw->auth_user));
    memset(gw->auth_pass, 0, sizeof(gw->auth_pass));
    if (user && user[0])
        strncpy(gw->auth_user, user, sizeof(gw->auth_user) - 1);
    if (pass && pass[0])
        strncpy(gw->auth_pass, pass, sizeof(gw->auth_pass) - 1);
    cmq_mutex_unlock(&gw->lock);
    return 0;
}

static void gw_auth_copy(cmq_gateway_t *gw, char *user, size_t ulen,
                          char *pass, size_t plen) {
    cmq_mutex_lock(&gw->lock);
    strncpy(user, gw->auth_user, ulen - 1);
    user[ulen - 1] = '\0';
    strncpy(pass, gw->auth_pass, plen - 1);
    pass[plen - 1] = '\0';
    cmq_mutex_unlock(&gw->lock);
}

static int gw_handshake(cmq_gateway_t *gw, int fd) {
    char user[256], pass[256];
    gw_auth_copy(gw, user, sizeof(user), pass, sizeof(pass));
    return cmq_peer_handshake(fd, user[0] ? user : NULL, pass[0] ? pass : NULL);
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
            if (gw->conns[i].connected && gw->conns[i].fd >= 0) {
                cmq_mutex_unlock(&gw->lock);
                return 0;
            }
            gw_slot_close_fd(gw, i);
            char addr_copy[CMQ_NODE_ADDR_SIZE];
            strncpy(addr_copy, addr, sizeof(addr_copy) - 1);
            addr_copy[sizeof(addr_copy) - 1] = '\0';
            int port_copy = port;
            size_t slot = i;
            cmq_mutex_unlock(&gw->lock);

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return -1;
            struct sockaddr_in sa = {0};
            sa.sin_family = AF_INET;
            sa.sin_port = htons((uint16_t)port_copy);
            if (inet_pton(AF_INET, addr_copy, &sa.sin_addr) != 1) {
                close(fd);
                return -1;
            }
            if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                                     CMQ_GW_CONNECT_MS) != 0) {
                close(fd);
                return -1;
            }
            if (gw_handshake(gw, fd) != 0) {
                close(fd);
                return -1;
            }
            set_nonblock(fd);
            cmq_mutex_lock(&gw->lock);
            /* Another thread may have published a live peer meanwhile. */
            for (size_t j = 0; j < gw->conn_count; j++) {
                if (strcmp(gw->conns[j].remote_cluster, cluster_name) == 0 &&
                    gw->conns[j].connected && gw->conns[j].fd >= 0) {
                    cmq_mutex_unlock(&gw->lock);
                    close(fd);
                    return 0;
                }
            }
            if (slot < gw->conn_count &&
                strcmp(gw->conns[slot].remote_cluster, cluster_name) == 0) {
                gw_slot_close_fd(gw, slot);
                gw_slot_install(gw, slot, cluster_name, addr_copy, port_copy, fd);
                cmq_mutex_unlock(&gw->lock);
                return 0;
            }
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return -1;
        }
    }

    if (gw->conn_count >= CMQ_GW_MAX_CONNECTIONS) {
        /* Reuse a dead/empty slot rather than failing when the table is full. */
        int slot = -1;
        for (size_t i = 0; i < gw->conn_count; i++) {
            if (!gw->conns[i].connected ||
                gw->conns[i].remote_cluster[0] == '\0') {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            cmq_mutex_unlock(&gw->lock);
            return -1;
        }
        char addr_copy[CMQ_NODE_ADDR_SIZE];
        strncpy(addr_copy, addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';
        int port_copy = port;
        cmq_mutex_unlock(&gw->lock);

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port_copy);
        if (inet_pton(AF_INET, addr_copy, &sa.sin_addr) != 1) {
            close(fd);
            return -1;
        }
        if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                                 CMQ_GW_CONNECT_MS) != 0) {
            close(fd);
            return -1;
        }
        if (gw_handshake(gw, fd) != 0) {
            close(fd);
            return -1;
        }
        set_nonblock(fd);
        cmq_mutex_lock(&gw->lock);
        for (size_t j = 0; j < gw->conn_count; j++) {
            if (strcmp(gw->conns[j].remote_cluster, cluster_name) == 0 &&
                gw->conns[j].connected && gw->conns[j].fd >= 0) {
                cmq_mutex_unlock(&gw->lock);
                close(fd);
                return 0;
            }
        }
        if ((size_t)slot < gw->conn_count &&
            (!gw->conns[slot].connected ||
             gw->conns[slot].remote_cluster[0] == '\0' ||
             strcmp(gw->conns[slot].remote_cluster, cluster_name) == 0)) {
            gw_slot_close_fd(gw, (size_t)slot);
            gw_slot_install(gw, (size_t)slot, cluster_name, addr_copy,
                            port_copy, fd);
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return -1;
    }

    /* Copy addr/port then unlock for blocking connect+handshake. */
    char addr_copy[CMQ_NODE_ADDR_SIZE];
    strncpy(addr_copy, addr, sizeof(addr_copy) - 1);
    addr_copy[sizeof(addr_copy) - 1] = '\0';
    int port_copy = port;
    cmq_mutex_unlock(&gw->lock);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port_copy);
    if (inet_pton(AF_INET, addr_copy, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_GW_CONNECT_MS) != 0) {
        close(fd);
        return -1;
    }
    if (gw_handshake(gw, fd) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);

    cmq_mutex_lock(&gw->lock);
    /* Dedup after unlocked connect — avoid duplicate live peers. */
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0 &&
            gw->conns[i].connected && gw->conns[i].fd >= 0) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return 0;
        }
    }
    for (size_t i = 0; i < gw->conn_count; i++) {
        int same = (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0);
        if (!same && gw->conns[i].connected &&
            gw->conns[i].remote_cluster[0] != '\0')
            continue;
        gw_slot_close_fd(gw, i);
        gw_slot_install(gw, i, cluster_name, addr_copy, port_copy, fd);
        cmq_mutex_unlock(&gw->lock);
        return 0;
    }
    if (gw->conn_count >= CMQ_GW_MAX_CONNECTIONS) {
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return -1;
    }
    size_t idx = gw->conn_count++;
    gw_slot_install(gw, idx, cluster_name, addr_copy, port_copy, fd);

    cmq_mutex_unlock(&gw->lock);
    return 0;
}

int cmq_gateway_disconnect(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw || !cluster_name) return -1;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0) {
            gw_slot_close_fd(gw, i);
            while (gw->conn_count > 0) {
                cmq_gw_conn_t *last = &gw->conns[gw->conn_count - 1];
                if (last->remote_cluster[0] != '\0' || last->connected ||
                    last->fd >= 0)
                    break;
                gw->conn_count--;
            }
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return -1;
}

size_t cmq_gateway_forward(cmq_gateway_t *gw, const char *target_cluster,
                            const uint8_t *data, size_t len,
                            size_t *out_eagain) {
    if (out_eagain) *out_eagain = 0;
    if (!gw || !data || len == 0) return 0;
    int fds[CMQ_GW_MAX_CONNECTIONS];
    size_t idxs[CMQ_GW_MAX_CONNECTIONS];
    char clusters[CMQ_GW_MAX_CONNECTIONS][64];
    size_t n = 0;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count && n < CMQ_GW_MAX_CONNECTIONS; i++) {
        if (strcmp(gw->conns[i].remote_cluster, target_cluster) == 0 &&
            gw->conns[i].connected && gw->conns[i].fd >= 0) {
            fds[n] = gw->conns[i].fd;
            idxs[n] = i;
            memcpy(clusters[n], gw->conns[i].remote_cluster, 64);
            n++;
        }
    }
    cmq_mutex_unlock(&gw->lock);

    size_t sent = 0;
    size_t deferred = 0;
    for (size_t j = 0; j < n; j++) {
        size_t idx = idxs[j];
        int expect_fd = fds[j];
        cmq_mutex_lock(&gw->io_locks[idx]);
        /* Slot identity only under io_lock (writers use gw_slot_*). */
        int fd = -1;
        if (idx < CMQ_GW_MAX_CONNECTIONS &&
            gw->conns[idx].connected &&
            gw->conns[idx].fd == expect_fd &&
            memcmp(gw->conns[idx].remote_cluster, clusters[j], 64) == 0) {
            fd = expect_fd;
        }
        if (fd < 0) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            continue;
        }
        int wr = write_full(fd, data, len);
        if (wr == 0) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            sent++;
        } else if (wr == 1) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            deferred++;
        } else {
            /* Clear slot under io_lock before close (no fd reuse race). */
            if (gw->conns[idx].fd == fd) {
                gw->conns[idx].fd = -1;
                gw->conns[idx].connected = 0;
            }
            close(fd);
            cmq_mutex_unlock(&gw->io_locks[idx]);
        }
    }
    if (out_eagain) *out_eagain = deferred;
    return sent;
}

size_t cmq_gateway_broadcast(cmq_gateway_t *gw, const uint8_t *data, size_t len,
                              size_t *out_eagain) {
    if (out_eagain) *out_eagain = 0;
    if (!gw || !data || len == 0) return 0;
    int fds[CMQ_GW_MAX_CONNECTIONS];
    size_t idxs[CMQ_GW_MAX_CONNECTIONS];
    char clusters[CMQ_GW_MAX_CONNECTIONS][64];
    size_t n = 0;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count && n < CMQ_GW_MAX_CONNECTIONS; i++) {
        if (!gw->conns[i].connected || gw->conns[i].fd < 0) continue;
        fds[n] = gw->conns[i].fd;
        idxs[n] = i;
        memcpy(clusters[n], gw->conns[i].remote_cluster, 64);
        n++;
    }
    cmq_mutex_unlock(&gw->lock);

    size_t sent = 0;
    size_t deferred = 0;
    for (size_t j = 0; j < n; j++) {
        size_t idx = idxs[j];
        int expect_fd = fds[j];
        cmq_mutex_lock(&gw->io_locks[idx]);
        int fd = -1;
        if (idx < CMQ_GW_MAX_CONNECTIONS &&
            gw->conns[idx].connected &&
            gw->conns[idx].fd == expect_fd &&
            memcmp(gw->conns[idx].remote_cluster, clusters[j], 64) == 0) {
            fd = expect_fd;
        }
        if (fd < 0) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            continue;
        }
        int wr = write_full(fd, data, len);
        if (wr == 0) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            sent++;
        } else if (wr == 1) {
            cmq_mutex_unlock(&gw->io_locks[idx]);
            deferred++;
        } else {
            if (gw->conns[idx].fd == fd) {
                gw->conns[idx].fd = -1;
                gw->conns[idx].connected = 0;
            }
            close(fd);
            cmq_mutex_unlock(&gw->io_locks[idx]);
        }
    }
    if (out_eagain) *out_eagain = deferred;
    return sent;
}

size_t cmq_gateway_connection_count(cmq_gateway_t *gw) {
    if (!gw) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t c = 0;
    for (size_t i = 0; i < gw->conn_count; i++) {
        if (gw->conns[i].remote_cluster[0] != '\0')
            c++;
    }
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
