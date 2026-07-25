#define _POSIX_C_SOURCE 200809L
#include "cmq_gateway.h"
#include "cmq_route.h"
#include "cmq_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>

struct cmq_gateway {
    char local_cluster[64];
    char auth_user[256];
    char auth_pass[256];
    cmq_gw_conn_t conns[CMQ_GW_MAX_CONNECTIONS];
    size_t conn_count;
    cmq_gw_cluster_info_t clusters[CMQ_GW_MAX_CLUSTERS];
    uint32_t cluster_cancel_gen[CMQ_GW_MAX_CLUSTERS]; /* disconnect aborts dial */
    size_t cluster_count;
    cmq_mutex_t lock;
    cmq_mutex_t io_locks[CMQ_GW_MAX_CONNECTIONS]; /* per-slot write serialization */
    atomic_int in_flight; /* connect unlocked dial/handshake */
    atomic_int dying;
};

/* Caller holds gw->lock. */
static ssize_t gw_find_cluster(cmq_gateway_t *gw, const char *name) {
    if (!gw || !name) return -1;
    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, name) == 0)
            return (ssize_t)i;
    }
    return -1;
}

static uint32_t gw_cluster_cancel_snap(cmq_gateway_t *gw, const char *name) {
    ssize_t i = gw_find_cluster(gw, name);
    return i >= 0 ? gw->cluster_cancel_gen[i] : 0;
}

static int gw_cluster_cancel_changed(cmq_gateway_t *gw, const char *name,
                                      uint32_t gen) {
    ssize_t i = gw_find_cluster(gw, name);
    if (i < 0) return 1;
    return gw->cluster_cancel_gen[i] != gen;
}

#define CMQ_GW_WRITE_MS 50
#define CMQ_GW_CONNECT_MS 2000


static int gw_begin_op(cmq_gateway_t *gw) {
    if (atomic_load_explicit(&gw->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&gw->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&gw->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&gw->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void gw_end_op(cmq_gateway_t *gw) {
    atomic_fetch_sub_explicit(&gw->in_flight, 1, memory_order_acq_rel);
}

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
    snprintf(gw->conns[idx].remote_cluster,
             sizeof(gw->conns[idx].remote_cluster), "%s", cluster);
    snprintf(gw->conns[idx].remote_addr, sizeof(gw->conns[idx].remote_addr),
             "%s", addr);
    gw->conns[idx].remote_port = port;
    gw->conns[idx].fd = fd;
    gw->conns[idx].connected = 1;
    cmq_mutex_unlock(&gw->io_locks[idx]);
}

static int gw_fd_alive(int fd) {
    return cmq_tcp_fd_alive(fd);
}

/* Caller holds gw->lock on entry/exit. 1 = probed-live peer for cluster.
   Read fd/connected under io_lock (same as forward / route_peer_live). */
static int gw_has_live_peer(cmq_gateway_t *gw, const char *cluster_name) {
    for (;;) {
        size_t idx = (size_t)-1;
        int efd = -1;
        for (size_t j = 0; j < gw->conn_count; j++) {
            cmq_mutex_lock(&gw->io_locks[j]);
            int match = (strcmp(gw->conns[j].remote_cluster, cluster_name) == 0 &&
                         gw->conns[j].connected && gw->conns[j].fd >= 0);
            int fd = match ? gw->conns[j].fd : -1;
            cmq_mutex_unlock(&gw->io_locks[j]);
            if (match) {
                idx = j;
                efd = fd;
                break;
            }
        }
        if (idx == (size_t)-1) return 0;
        /* Probe under io_lock — never re-probe bare efd after unlock (EINTR
           sticky-alive / fd reuse). Decision to reclaim uses locked result. */
        if (idx < gw->conn_count) {
            cmq_mutex_lock(&gw->io_locks[idx]);
            int still =
                (strcmp(gw->conns[idx].remote_cluster, cluster_name) == 0 &&
                 gw->conns[idx].connected && gw->conns[idx].fd == efd);
            int live = still && gw_fd_alive(efd);
            int reclaim = still && !live;
            cmq_mutex_unlock(&gw->io_locks[idx]);
            if (live) return 1;
            if (reclaim)
                gw_slot_close_fd(gw, idx);
        }
    }
}

/* Caller holds gw->lock. True if cluster still maps to addr:port (add_remote
   may have rewritten the endpoint while connect was unlocked). */
static int gw_endpoint_current(cmq_gateway_t *gw, const char *cluster_name,
                               const char *addr, int port) {
    if (!gw || !cluster_name || !addr) return 0;
    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, cluster_name) == 0)
            return gw->clusters[i].port == port &&
                   strncmp(gw->clusters[i].addr, addr, CMQ_NODE_ADDR_SIZE) == 0;
    }
    return 0;
}

/* Claim slot for a freshly dialed fd. Caller holds gw->lock on entry/exit.
   0 = installed; 1 = keep existing live peer (caller closes new fd);
   -1 = slot not reclaimable. Never gw_slot_close a probed-live same-cluster peer. */
static int gw_slot_claim_install(cmq_gateway_t *gw, size_t slot,
                                 const char *cluster_name, const char *addr,
                                 int port, int fd) {
    if (slot >= gw->conn_count) return -1;
    cmq_mutex_lock(&gw->io_locks[slot]);
    int connected = gw->conns[slot].connected;
    int empty = (gw->conns[slot].remote_cluster[0] == '\0');
    int same = (strcmp(gw->conns[slot].remote_cluster, cluster_name) == 0);
    int efd = gw->conns[slot].fd;
    cmq_mutex_unlock(&gw->io_locks[slot]);

    if (same && connected && efd >= 0) {
        if (slot >= gw->conn_count) return -1;
        cmq_mutex_lock(&gw->io_locks[slot]);
        int still =
            (strcmp(gw->conns[slot].remote_cluster, cluster_name) == 0 &&
             gw->conns[slot].fd == efd);
        int live = still && gw_fd_alive(efd);
        cmq_mutex_unlock(&gw->io_locks[slot]);
        if (!still) return -1;
        if (live) return 1;
        /* Locked probe said dead — reclaim; do not re-probe unlocked efd. */
        gw_slot_close_fd(gw, slot);
        gw_slot_install(gw, slot, cluster_name, addr, port, fd);
        return 0;
    }
    if (!connected || empty || same) {
        gw_slot_close_fd(gw, slot);
        gw_slot_install(gw, slot, cluster_name, addr, port, fd);
        return 0;
    }
    /* Cross-cluster sticky: reclaim only if TCP is dead (never steal live). */
    if (efd >= 0) {
        char other[64];
        if (slot >= gw->conn_count) return -1;
        cmq_mutex_lock(&gw->io_locks[slot]);
        memcpy(other, gw->conns[slot].remote_cluster, sizeof(other));
        other[sizeof(other) - 1] = '\0';
        int still = (gw->conns[slot].fd == efd &&
                     strcmp(gw->conns[slot].remote_cluster, other) == 0);
        int live = still && gw_fd_alive(efd);
        cmq_mutex_unlock(&gw->io_locks[slot]);
        if (still && !live) {
            gw_slot_close_fd(gw, slot);
            gw_slot_install(gw, slot, cluster_name, addr, port, fd);
            return 0;
        }
    }
    return -1;
}

/* Place dialed fd: scan reclaimable slots, else append. Caller holds gw->lock.
   0 = installed (table owns fd); 1 = live peer already (caller closes fd);
   -1 = cannot place (caller closes fd). */
static int gw_place_dialed_fd(cmq_gateway_t *gw, const char *cluster_name,
                              const char *addr, int port, int fd) {
    for (size_t i = 0; i < gw->conn_count; i++) {
        int rc = gw_slot_claim_install(gw, i, cluster_name, addr, port, fd);
        if (rc == 0) return 0;
        if (rc == 1) return 1;
    }
    if (gw_has_live_peer(gw, cluster_name)) return 1;
    if (!gw_endpoint_current(gw, cluster_name, addr, port)) return -1;
    if (gw->conn_count >= CMQ_GW_MAX_CONNECTIONS) return -1;
    size_t idx = gw->conn_count++;
    gw_slot_install(gw, idx, cluster_name, addr, port, fd);
    return 0;
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
    /* local_cluster[64] — reject before truncate-collide. */
    if (strnlen(local_cluster, 64) >= 64)
        return NULL;
    cmq_gateway_t *gw = calloc(1, sizeof(cmq_gateway_t));
    if (!gw) return NULL;
    snprintf(gw->local_cluster, sizeof(gw->local_cluster), "%s", local_cluster);
    atomic_init(&gw->in_flight, 0);
    atomic_init(&gw->dying, 0);
    cmq_mutex_init(&gw->lock);
    for (size_t i = 0; i < CMQ_GW_MAX_CONNECTIONS; i++)
        cmq_mutex_init(&gw->io_locks[i]);
    return gw;
}

void cmq_gateway_destroy(cmq_gateway_t *gw) {
    if (!gw) return;
    atomic_store_explicit(&gw->dying, 1, memory_order_release);
    while (atomic_load_explicit(&gw->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->conn_count; i++)
        gw_slot_close_fd(gw, i);
    cmq_mutex_unlock(&gw->lock);
    for (size_t i = 0; i < CMQ_GW_MAX_CONNECTIONS; i++)
        cmq_mutex_destroy(&gw->io_locks[i]);
    cmq_mutex_destroy(&gw->lock);
    free(gw);
}

static int gw_set_auth_impl(cmq_gateway_t *gw, const char *user, const char *pass) {
    if (!gw) return -1;
    /* Align with CONNECT + config: wire caps creds at 255 bytes. */
    if ((user && strnlen(user, sizeof(gw->auth_user)) >= sizeof(gw->auth_user)) ||
        (pass && strnlen(pass, sizeof(gw->auth_pass)) >= sizeof(gw->auth_pass)))
        return -1;
    cmq_mutex_lock(&gw->lock);
    memset(gw->auth_user, 0, sizeof(gw->auth_user));
    memset(gw->auth_pass, 0, sizeof(gw->auth_pass));
    if (user && user[0])
        snprintf(gw->auth_user, sizeof(gw->auth_user), "%s", user);
    if (pass && pass[0])
        snprintf(gw->auth_pass, sizeof(gw->auth_pass), "%s", pass);
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
    return cmq_peer_handshake(fd, user[0] ? user : NULL, pass[0] ? pass : NULL, 0);
}

static int gw_add_remote_impl(cmq_gateway_t *gw, const char *cluster_name,
                            const char *addr, int port) {
    if (!gw || !cluster_name || !addr) return -1;
    /* name[64] — reject before truncate can collide cluster identities. */
    if (strnlen(cluster_name, 64) >= 64 ||
        strnlen(addr, CMQ_NODE_ADDR_SIZE) >= CMQ_NODE_ADDR_SIZE)
        return -1;
    if (port <= 0 || port > 65535) return -1;
    cmq_mutex_lock(&gw->lock);

    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, cluster_name) == 0) {
            int addr_changed =
                (strcmp(gw->clusters[i].addr, addr) != 0) ||
                (gw->clusters[i].port != port);
            /* snprintf clears leftover bytes when the new addr is shorter. */
            snprintf(gw->clusters[i].addr, sizeof(gw->clusters[i].addr),
                     "%s", addr);
            gw->clusters[i].port = port;
            gw->clusters[i].known = 1;
            if (addr_changed) {
                /* Invalidate live TCP + cancel in-flight dials to the old ep. */
                gw->cluster_cancel_gen[i]++;
                for (size_t j = 0; j < gw->conn_count; j++) {
                    cmq_mutex_lock(&gw->io_locks[j]);
                    int match =
                        (strcmp(gw->conns[j].remote_cluster, cluster_name) == 0);
                    cmq_mutex_unlock(&gw->io_locks[j]);
                    if (match)
                        gw_slot_close_fd(gw, j);
                }
            }
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }

    if (gw->cluster_count >= CMQ_GW_MAX_CLUSTERS) {
        cmq_mutex_unlock(&gw->lock);
        return -1;
    }

    cmq_gw_cluster_info_t *ci = &gw->clusters[gw->cluster_count++];
    snprintf(ci->name, sizeof(ci->name), "%s", cluster_name);
    snprintf(ci->addr, sizeof(ci->addr), "%s", addr);
    ci->port = port;
    ci->known = 1;

    cmq_mutex_unlock(&gw->lock);
    return 0;
}

static int gw_connect_remote_impl(cmq_gateway_t *gw, const char *cluster_name) {
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
        cmq_mutex_lock(&gw->io_locks[i]);
        int match = (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0);
        int connected = gw->conns[i].connected;
        int efd = gw->conns[i].fd;
        int same_ep = match && connected && efd >= 0 &&
                      gw->conns[i].remote_port == port &&
                      strncmp(gw->conns[i].remote_addr, addr,
                              CMQ_NODE_ADDR_SIZE) == 0;
        cmq_mutex_unlock(&gw->io_locks[i]);
        if (!match) continue;
        if (same_ep) {
            size_t idx = i;
            if (idx < gw->conn_count) {
                cmq_mutex_lock(&gw->io_locks[idx]);
                int same =
                    (strcmp(gw->conns[idx].remote_cluster, cluster_name) == 0 &&
                     gw->conns[idx].connected && gw->conns[idx].fd == efd);
                /* Probe under io_lock — do not re-probe bare efd after unlock. */
                int live = same && gw_fd_alive(efd);
                int reclaim = same && !live;
                cmq_mutex_unlock(&gw->io_locks[idx]);
                if (live) {
                    cmq_mutex_unlock(&gw->lock);
                    return 0;
                }
                if (reclaim)
                    gw_slot_close_fd(gw, idx);
            }
            /* Rescan — avoid closing a peer installed while unlocked. */
            i = (size_t)-1;
            continue;
        }
        gw_slot_close_fd(gw, i);
        char addr_copy[CMQ_NODE_ADDR_SIZE];
        strncpy(addr_copy, addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';
        int port_copy = port;
        size_t slot = i;
        uint32_t cgen = gw_cluster_cancel_snap(gw, cluster_name);
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
        if (gw_cluster_cancel_changed(gw, cluster_name, cgen)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return -1;
        }
        /* Another thread may have published a live peer meanwhile. */
        if (gw_has_live_peer(gw, cluster_name)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return 0;
        }
        if (!gw_endpoint_current(gw, cluster_name, addr_copy, port_copy)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return -1;
        }
        /* Prefer closed slot; if stolen/truncated, scan+append like cold path. */
        int prc = -1;
        if (slot < gw->conn_count)
            prc = gw_slot_claim_install(gw, slot, cluster_name, addr_copy,
                                       port_copy, fd);
        if (prc != 0 && prc != 1)
            prc = gw_place_dialed_fd(gw, cluster_name, addr_copy, port_copy, fd);
        if (prc == 0) {
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
        if (prc == 1) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return 0;
        }
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return -1;
    }

    if (gw->conn_count >= CMQ_GW_MAX_CONNECTIONS) {
        /* Reuse a dead/empty slot rather than failing when the table is full.
           Also reclaim sticky connected=1 slots whose TCP is already dead. */
        int slot = -1;
        for (size_t i = 0; i < gw->conn_count; i++) {
            cmq_mutex_lock(&gw->io_locks[i]);
            int reusable = (!gw->conns[i].connected ||
                            gw->conns[i].remote_cluster[0] == '\0' ||
                            gw->conns[i].fd < 0);
            cmq_mutex_unlock(&gw->io_locks[i]);
            if (reusable) {
                slot = (int)i;
                break;
            }
        }
        for (size_t i = 0; slot < 0 && i < gw->conn_count; ) {
            cmq_mutex_lock(&gw->io_locks[i]);
            int connected = gw->conns[i].connected;
            int efd = gw->conns[i].fd;
            cmq_mutex_unlock(&gw->io_locks[i]);
            if (!connected || efd < 0) {
                slot = (int)i;
                break;
            }
            if (i >= gw->conn_count)
                break;
            cmq_mutex_lock(&gw->io_locks[i]);
            int same = (gw->conns[i].fd == efd);
            int live = same && gw_fd_alive(efd);
            cmq_mutex_unlock(&gw->io_locks[i]);
            if (!live && same) {
                gw_slot_close_fd(gw, i);
                slot = (int)i;
                break;
            }
            if (live) {
                i++;
                continue;
            }
            /* Slot mutated — rescan. */
            i = 0;
        }
        if (slot < 0) {
            cmq_mutex_unlock(&gw->lock);
            return -1;
        }
        char addr_copy[CMQ_NODE_ADDR_SIZE];
        strncpy(addr_copy, addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';
        int port_copy = port;
        uint32_t cgen = gw_cluster_cancel_snap(gw, cluster_name);
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
        if (gw_cluster_cancel_changed(gw, cluster_name, cgen)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return -1;
        }
        if (gw_has_live_peer(gw, cluster_name)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return 0;
        }
        if (!gw_endpoint_current(gw, cluster_name, addr_copy, port_copy)) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
            return -1;
        }
        int prc = -1;
        if ((size_t)slot < gw->conn_count)
            prc = gw_slot_claim_install(gw, (size_t)slot, cluster_name,
                                       addr_copy, port_copy, fd);
        if (prc != 0 && prc != 1)
            prc = gw_place_dialed_fd(gw, cluster_name, addr_copy, port_copy, fd);
        if (prc == 0) {
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
        if (prc == 1) {
            cmq_mutex_unlock(&gw->lock);
            close(fd);
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
    uint32_t cgen = gw_cluster_cancel_snap(gw, cluster_name);
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
    if (gw_cluster_cancel_changed(gw, cluster_name, cgen)) {
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return -1;
    }
    /* Dedup after unlocked connect — avoid duplicate live peers. */
    if (gw_has_live_peer(gw, cluster_name)) {
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return 0;
    }
    if (!gw_endpoint_current(gw, cluster_name, addr_copy, port_copy)) {
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return -1;
    }
    int prc = gw_place_dialed_fd(gw, cluster_name, addr_copy, port_copy, fd);
    if (prc == 0) {
        cmq_mutex_unlock(&gw->lock);
        return 0;
    }
    if (prc == 1) {
        cmq_mutex_unlock(&gw->lock);
        close(fd);
        return 0;
    }
    cmq_mutex_unlock(&gw->lock);
    close(fd);
    return -1;
}

static int gw_disconnect_impl(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw || !cluster_name) return -1;
    cmq_mutex_lock(&gw->lock);
    /* Always bump cancel_gen so an in-flight dial cannot reinstall. */
    ssize_t ci = gw_find_cluster(gw, cluster_name);
    if (ci >= 0)
        gw->cluster_cancel_gen[ci]++;
    int closed = 0;
    /* Close every matching slot — multi-conn / retry residue must not
       keep forwarding after disconnect. */
    for (size_t i = 0; i < gw->conn_count; i++) {
        cmq_mutex_lock(&gw->io_locks[i]);
        int match = (strcmp(gw->conns[i].remote_cluster, cluster_name) == 0);
        cmq_mutex_unlock(&gw->io_locks[i]);
        if (match) {
            gw_slot_close_fd(gw, i);
            closed = 1;
        }
    }
    while (gw->conn_count > 0) {
        size_t last = gw->conn_count - 1;
        cmq_mutex_lock(&gw->io_locks[last]);
        int empty = (gw->conns[last].remote_cluster[0] == '\0' &&
                     !gw->conns[last].connected &&
                     gw->conns[last].fd < 0);
        cmq_mutex_unlock(&gw->io_locks[last]);
        if (!empty)
            break;
        gw->conn_count--;
    }
    cmq_mutex_unlock(&gw->lock);
    /* Known cluster with no live slot: cancel still succeeded. */
    return (closed || ci >= 0) ? 0 : -1;
}

size_t cmq_gateway_forward(cmq_gateway_t *gw, const char *target_cluster,
                            const uint8_t *data, size_t len,
                            size_t *out_eagain) {
    if (out_eagain) *out_eagain = 0;
    if (!gw || !data || len == 0) return 0;
    if (gw_begin_op(gw) != 0) return 0;
    int fds[CMQ_GW_MAX_CONNECTIONS];
    size_t idxs[CMQ_GW_MAX_CONNECTIONS];
    char clusters[CMQ_GW_MAX_CONNECTIONS][64];
    size_t n = 0;
    /* Snapshot under gw→io so conn_count cannot grow mid-scan. */
    cmq_mutex_lock(&gw->lock);
    size_t nslots = gw->conn_count;
    for (size_t i = 0; i < nslots && n < CMQ_GW_MAX_CONNECTIONS; i++) {
        cmq_mutex_lock(&gw->io_locks[i]);
        if (strcmp(gw->conns[i].remote_cluster, target_cluster) == 0 &&
            gw->conns[i].connected && gw->conns[i].fd >= 0) {
            fds[n] = gw->conns[i].fd;
            idxs[n] = i;
            memcpy(clusters[n], gw->conns[i].remote_cluster, 64);
            n++;
        }
        cmq_mutex_unlock(&gw->io_locks[i]);
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
            /* Snapshotted peer vanished — count undelivered (align route). */
            cmq_mutex_unlock(&gw->io_locks[idx]);
            deferred++;
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
            /* Clear identity too — sticky named tombstones fill the table.
               Count as undelivered for drop/observability stats. */
            if (gw->conns[idx].fd == fd) {
                memset(&gw->conns[idx], 0, sizeof(gw->conns[idx]));
                gw->conns[idx].fd = -1;
            }
            close(fd);
            cmq_mutex_unlock(&gw->io_locks[idx]);
            deferred++;
        }
    }
    if (out_eagain) *out_eagain = deferred;
    gw_end_op(gw);
    return sent;
}

size_t cmq_gateway_broadcast(cmq_gateway_t *gw, const uint8_t *data, size_t len,
                              size_t *out_eagain) {
    if (out_eagain) *out_eagain = 0;
    if (!gw || !data || len == 0) return 0;
    if (gw_begin_op(gw) != 0) return 0;
    int fds[CMQ_GW_MAX_CONNECTIONS];
    size_t idxs[CMQ_GW_MAX_CONNECTIONS];
    char clusters[CMQ_GW_MAX_CONNECTIONS][64];
    size_t n = 0;
    cmq_mutex_lock(&gw->lock);
    size_t nslots = gw->conn_count;
    for (size_t i = 0; i < nslots && n < CMQ_GW_MAX_CONNECTIONS; i++) {
        cmq_mutex_lock(&gw->io_locks[i]);
        if (gw->conns[i].connected && gw->conns[i].fd >= 0) {
            fds[n] = gw->conns[i].fd;
            idxs[n] = i;
            memcpy(clusters[n], gw->conns[i].remote_cluster, 64);
            n++;
        }
        cmq_mutex_unlock(&gw->io_locks[i]);
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
            deferred++;
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
                memset(&gw->conns[idx], 0, sizeof(gw->conns[idx]));
                gw->conns[idx].fd = -1;
            }
            close(fd);
            cmq_mutex_unlock(&gw->io_locks[idx]);
            deferred++;
        }
    }
    if (out_eagain) *out_eagain = deferred;
    gw_end_op(gw);
    return sent;
}

static size_t gw_connection_count_impl(cmq_gateway_t *gw) {
    if (!gw) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t c = 0;
    for (size_t i = 0; i < gw->conn_count; i++) {
        /* Identity published/cleared under io_lock — match forward/add_remote. */
        cmq_mutex_lock(&gw->io_locks[i]);
        int named = (gw->conns[i].remote_cluster[0] != '\0');
        cmq_mutex_unlock(&gw->io_locks[i]);
        if (named)
            c++;
    }
    cmq_mutex_unlock(&gw->lock);
    return c;
}

static size_t gw_known_cluster_count_impl(cmq_gateway_t *gw) {
    if (!gw) return 0;
    cmq_mutex_lock(&gw->lock);
    size_t c = gw->cluster_count;
    cmq_mutex_unlock(&gw->lock);
    return c;
}

static int gw_get_cluster_impl(cmq_gateway_t *gw, const char *name,
                             cmq_gw_cluster_info_t *out) {
    if (!gw || !name || !out) return -1;
    cmq_mutex_lock(&gw->lock);
    for (size_t i = 0; i < gw->cluster_count; i++) {
        if (strcmp(gw->clusters[i].name, name) == 0) {
            *out = gw->clusters[i];
            cmq_mutex_unlock(&gw->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&gw->lock);
    return -1;
}



int cmq_gateway_set_auth(cmq_gateway_t *gw, const char *user, const char *pass) {
    if (!gw) return -1;
    if (gw_begin_op(gw) != 0) return -1;
    int rc = gw_set_auth_impl(gw, user, pass);
    gw_end_op(gw);
    return rc;
}

int cmq_gateway_add_remote(cmq_gateway_t *gw, const char *cluster_name,
                            const char *addr, int port) {
    if (!gw || !cluster_name || !addr) return -1;
    if (gw_begin_op(gw) != 0) return -1;
    int rc = gw_add_remote_impl(gw, cluster_name, addr, port);
    gw_end_op(gw);
    return rc;
}

int cmq_gateway_disconnect(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw || !cluster_name) return -1;
    if (gw_begin_op(gw) != 0) return -1;
    int rc = gw_disconnect_impl(gw, cluster_name);
    gw_end_op(gw);
    return rc;
}

size_t cmq_gateway_connection_count(cmq_gateway_t *gw) {
    if (!gw) return 0;
    if (gw_begin_op(gw) != 0) return 0;
    size_t c = gw_connection_count_impl(gw);
    gw_end_op(gw);
    return c;
}

size_t cmq_gateway_known_cluster_count(cmq_gateway_t *gw) {
    if (!gw) return 0;
    if (gw_begin_op(gw) != 0) return 0;
    size_t c = gw_known_cluster_count_impl(gw);
    gw_end_op(gw);
    return c;
}

int cmq_gateway_get_cluster(cmq_gateway_t *gw, const char *name,
                             cmq_gw_cluster_info_t *out) {
    if (!gw || !name || !out) return -1;
    if (gw_begin_op(gw) != 0) return -1;
    int rc = gw_get_cluster_impl(gw, name, out);
    gw_end_op(gw);
    return rc;
}

int cmq_gateway_connect_remote(cmq_gateway_t *gw, const char *cluster_name) {
    if (!gw) return -1;
    if (gw_begin_op(gw) != 0) return -1;
    int rc = gw_connect_remote_impl(gw, cluster_name);
    gw_end_op(gw);
    return rc;
}
