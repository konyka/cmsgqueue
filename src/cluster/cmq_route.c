#define _POSIX_C_SOURCE 200809L
#include "cmq_route.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_thread.h"
#include "cmq_types.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#define CMQ_ROUTE_MAX_CONNS 32
#define CMQ_ROUTE_HANDSHAKE_MS 3000
#define CMQ_ROUTE_CONNECT_MS 2000
/* Bound worker-thread stalls on partial route writes (was HANDSHAKE_MS). */
#define CMQ_ROUTE_WRITE_POLL_MS 50

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_block(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

/* Nonblocking connect with poll deadline, then restore blocking for handshake. */
int cmq_connect_timeout(int fd, const struct sockaddr *sa, socklen_t slen,
                         int timeout_ms) {
    set_nonblock(fd);
    int rc = connect(fd, sa, slen);
    if (rc != 0 && errno != EINPROGRESS) return -1;
    if (rc != 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        for (;;) {
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (pr == 0) return -1;
            break;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)
            return -1;
    }
    set_block(fd);
    return 0;
}

/* Blocking CONNECT + CONNACK so the peer accepts subsequent PUBLISH frames.
   Must run before set_nonblock. Skips INFO (sent before CONNACK). */
int cmq_peer_handshake(int fd, const char *auth_user, const char *auth_pass,
                        uint16_t flags) {
    uint8_t payload[520];
    uint16_t ulen = 0, pwen = 0;
    if (auth_user) {
        size_t n = strlen(auth_user);
        ulen = (uint16_t)(n > 255 ? 255 : n);
    }
    if (auth_pass) {
        size_t n = strlen(auth_pass);
        pwen = (uint16_t)(n > 255 ? 255 : n);
    }
    payload[0] = (uint8_t)(ulen >> 8);
    payload[1] = (uint8_t)ulen;
    payload[2] = (uint8_t)(pwen >> 8);
    payload[3] = (uint8_t)pwen;
    if (ulen) memcpy(payload + 4, auth_user, ulen);
    if (pwen) memcpy(payload + 4 + ulen, auth_pass, pwen);
    size_t plen = 4 + (size_t)ulen + (size_t)pwen;

    uint8_t buf[600];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNECT,
                                   flags, payload, plen);
    if (len == 0) return -1;

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }

    uint8_t rbuf[2048];
    size_t rlen = 0;
    int waited_ms = 0;
    while (waited_ms < CMQ_ROUTE_HANDSHAKE_MS) {
        while (rlen >= sizeof(cmq_frame_hdr_t)) {
            const uint8_t *hb = rbuf;
            if (hb[0] != CMQ_PROTO_MAGIC_0 || hb[1] != CMQ_PROTO_MAGIC_1)
                return -1;
            if (hb[2] != CMQ_PROTO_VERSION)
                return -1;
            uint8_t op = hb[4];
            uint32_t plen_f = (uint32_t)hb[5] | ((uint32_t)hb[6] << 8) |
                               ((uint32_t)hb[7] << 16) | ((uint32_t)hb[8] << 24);
            size_t need = sizeof(cmq_frame_hdr_t) + (size_t)plen_f;
            if (need > sizeof(rbuf)) return -1;
            if (rlen < need) break;

            if (op == (uint8_t)CMQ_OP_CONNACK) {
                if (plen_f < 1) return -1;
                return rbuf[sizeof(cmq_frame_hdr_t)] == 0 ? 0 : -1;
            }
            if (op != (uint8_t)CMQ_OP_INFO)
                return -1;
            /* Skip INFO frames before CONNACK. */
            memmove(rbuf, rbuf + need, rlen - need);
            rlen -= need;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            waited_ms += 100;
            continue;
        }
        if (rlen >= sizeof(rbuf)) return -1;
        ssize_t n = read(fd, rbuf + rlen, sizeof(rbuf) - rlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        rlen += (size_t)n;
    }
    return -1;
}

/* 0 = full write, 1 = EAGAIN with zero progress (keep fd), -1 = hard failure.
   Partial progress + EAGAIN polls POLLOUT (EINTR-safe, bounded stall rounds)
   rather than closing mid-frame. */
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
                    int pr = poll(&pfd, 1, CMQ_ROUTE_WRITE_POLL_MS);
                    if (pr > 0) {
                        stall_rounds = 0;
                        break;
                    }
                    if (pr < 0 && errno == EINTR) continue;
                    if (pr == 0 && ++stall_rounds < 4)
                        continue; /* ~200ms total before giving up */
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
    cmq_mutex_t io_locks[CMQ_ROUTE_MAX_CONNS]; /* per-slot write serialization */
};

/* Drop slot fd. Owned → close; borrowed inbound → SHUT_RDWR so the server
   client_read_cb sees EOF (same as write-path death). Slot replace/reconnect
   must not leave zombie is_route clients. */
static void conn_drop_fd(cmq_route_conn_t *c) {
    if (!c) return;
    if (c->fd >= 0) {
        if (c->fd_owned)
            close(c->fd);
        else
            (void)shutdown(c->fd, SHUT_RDWR);
    }
    c->fd = -1;
    c->connected = 0;
    c->fd_owned = 0;
}

/* Caller must hold pool->lock. Snapshot under io_lock (order: pool→io). */
static void route_slot_snap(cmq_route_pool_t *pool, size_t idx,
                             int *connected, int *fd, int *fd_owned,
                             char *remote_id_out) {
    if (!pool || idx >= CMQ_ROUTE_MAX_CONNS) {
        if (connected) *connected = 0;
        if (fd) *fd = -1;
        if (fd_owned) *fd_owned = 0;
        if (remote_id_out) remote_id_out[0] = '\0';
        return;
    }
    cmq_mutex_lock(&pool->io_locks[idx]);
    if (connected) *connected = pool->conns[idx].connected;
    if (fd) *fd = pool->conns[idx].fd;
    if (fd_owned) *fd_owned = pool->conns[idx].fd_owned;
    if (remote_id_out) {
        memcpy(remote_id_out, pool->conns[idx].remote_id, CMQ_NODE_ID_SIZE);
        remote_id_out[CMQ_NODE_ID_SIZE - 1] = '\0';
    }
    cmq_mutex_unlock(&pool->io_locks[idx]);
}

/* Caller must hold pool->lock. Order: pool->lock → io_lock. */
static void route_slot_close(cmq_route_pool_t *pool, size_t idx) {
    if (!pool || idx >= CMQ_ROUTE_MAX_CONNS) return;
    cmq_mutex_lock(&pool->io_locks[idx]);
    conn_drop_fd(&pool->conns[idx]);
    memset(&pool->conns[idx], 0, sizeof(pool->conns[idx]));
    cmq_mutex_unlock(&pool->io_locks[idx]);
}

/* Zero-timeout TCP liveness — sticky connected after peer death. */
static int route_fd_alive(int fd) {
    if (fd < 0) return 0;
    struct pollfd pfd = { .fd = fd, .events = 0 };
    int pr = poll(&pfd, 1, 0);
    return pr >= 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
}

/* Empty remote_addr = inbound/placeholder (no configured endpoint). */
static int route_ep_same(const char *slot_addr, int slot_port,
                          const char *addr, int port) {
    if (!slot_addr || slot_addr[0] == '\0')
        return 1;
    return slot_port == port &&
           strncmp(slot_addr, addr, CMQ_NODE_ADDR_SIZE) == 0;
}

/* Publish a peer into a slot. Caller holds pool->lock.
   connected=0 stages inbound until handshake is drained.
   addr may be NULL for inbound/placeholder (no endpoint sticky). */
static void route_slot_install(cmq_route_pool_t *pool, size_t idx,
                                const char *node_id, int fd, int fd_owned,
                                int connected, const char *addr, int port) {
    cmq_mutex_lock(&pool->io_locks[idx]);
    memset(&pool->conns[idx], 0, sizeof(pool->conns[idx]));
    strncpy(pool->conns[idx].remote_id, node_id, CMQ_NODE_ID_SIZE - 1);
    if (addr && addr[0]) {
        strncpy(pool->conns[idx].remote_addr, addr, CMQ_NODE_ADDR_SIZE - 1);
        pool->conns[idx].remote_port = port;
    }
    pool->conns[idx].fd = fd;
    pool->conns[idx].connected = connected ? 1 : 0;
    pool->conns[idx].fd_owned = fd_owned ? 1 : 0;
    cmq_mutex_unlock(&pool->io_locks[idx]);
}

cmq_route_pool_t *cmq_route_pool_create(cmq_cluster_t *cluster) {
    cmq_route_pool_t *p = calloc(1, sizeof(cmq_route_pool_t));
    if (!p) return NULL;
    p->cluster = cluster;
    p->conn_count = 0;
    p->interest_count = 0;
    cmq_mutex_init(&p->lock);
    for (size_t i = 0; i < CMQ_ROUTE_MAX_CONNS; i++)
        cmq_mutex_init(&p->io_locks[i]);
    return p;
}

void cmq_route_pool_destroy(cmq_route_pool_t *pool) {
    if (!pool) return;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++)
        route_slot_close(pool, i);
    cmq_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < CMQ_ROUTE_MAX_CONNS; i++)
        cmq_mutex_destroy(&pool->io_locks[i]);
    cmq_mutex_destroy(&pool->lock);
    free(pool);
}

int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass) {
    if (!pool || !node_id || !addr) return -1;

    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        int match = (strcmp(pool->conns[i].remote_id, node_id) == 0);
        int efd = pool->conns[i].fd;
        char ep[CMQ_NODE_ADDR_SIZE];
        int eport = 0;
        if (match && efd >= 0) {
            memcpy(ep, pool->conns[i].remote_addr, CMQ_NODE_ADDR_SIZE);
            ep[CMQ_NODE_ADDR_SIZE - 1] = '\0';
            eport = pool->conns[i].remote_port;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        /* Live or staged (fd>=0). Sticky only for same configured endpoint;
           inbound/empty addr keeps any alive peer (do not steal staged). */
        if (match && efd >= 0) {
            size_t idx = i;
            int same_ep = route_ep_same(ep, eport, addr, port);
            cmq_mutex_unlock(&pool->lock);
            int alive = route_fd_alive(efd) && same_ep;
            cmq_mutex_lock(&pool->lock);
            if (idx < pool->conn_count) {
                cmq_mutex_lock(&pool->io_locks[idx]);
                int same = (strcmp(pool->conns[idx].remote_id, node_id) == 0 &&
                            pool->conns[idx].fd == efd);
                char ep2[CMQ_NODE_ADDR_SIZE];
                int eport2 = 0;
                if (same) {
                    memcpy(ep2, pool->conns[idx].remote_addr, CMQ_NODE_ADDR_SIZE);
                    ep2[CMQ_NODE_ADDR_SIZE - 1] = '\0';
                    eport2 = pool->conns[idx].remote_port;
                }
                cmq_mutex_unlock(&pool->io_locks[idx]);
                /* Re-check identity after probe — fd recycle must not fake success. */
                if (alive && same && route_ep_same(ep2, eport2, addr, port)) {
                    cmq_mutex_unlock(&pool->lock);
                    return 0;
                }
                /* Drop dead or wrong-endpoint outbound; never steal inbound. */
                if (same &&
                    (!route_fd_alive(efd) ||
                     (ep2[0] != '\0' && !route_ep_same(ep2, eport2, addr, port))))
                    route_slot_close(pool, idx);
            }
            break;
        }
    }
    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        /* Full: same-id, empty, or fd<0+!connected tombstone — not staged. */
        int usable = 0;
        for (size_t i = 0; i < pool->conn_count; i++) {
            char rid[CMQ_NODE_ID_SIZE];
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, rid);
            if (strcmp(rid, node_id) == 0 || rid[0] == '\0' ||
                (!connected && efd < 0)) {
                usable = 1;
                break;
            }
        }
        if (!usable) {
            cmq_mutex_unlock(&pool->lock);
            return -1;
        }
    }
    cmq_mutex_unlock(&pool->lock);

    /* Connect + handshake outside the pool lock so broadcast can proceed. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_ROUTE_CONNECT_MS) != 0) {
        close(fd);
        return -1;
    }
    if (cmq_peer_handshake(fd, auth_user, auth_pass, CMQ_FLAG_ROUTE) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);

    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, rid);
        if (strcmp(rid, node_id) != 0) continue;
        /* Another thread may have restored a live or staged egress. */
        if (efd >= 0) {
            size_t idx = i;
            char ep[CMQ_NODE_ADDR_SIZE];
            int eport = 0;
            cmq_mutex_lock(&pool->io_locks[idx]);
            memcpy(ep, pool->conns[idx].remote_addr, CMQ_NODE_ADDR_SIZE);
            ep[CMQ_NODE_ADDR_SIZE - 1] = '\0';
            eport = pool->conns[idx].remote_port;
            cmq_mutex_unlock(&pool->io_locks[idx]);
            int same_ep = route_ep_same(ep, eport, addr, port);
            cmq_mutex_unlock(&pool->lock);
            int alive = route_fd_alive(efd) && same_ep;
            cmq_mutex_lock(&pool->lock);
            if (idx < pool->conn_count) {
                char rid2[CMQ_NODE_ID_SIZE];
                int f2 = -1;
                route_slot_snap(pool, idx, NULL, &f2, NULL, rid2);
                char ep2[CMQ_NODE_ADDR_SIZE];
                int eport2 = 0;
                if (strcmp(rid2, node_id) == 0 && f2 == efd) {
                    cmq_mutex_lock(&pool->io_locks[idx]);
                    memcpy(ep2, pool->conns[idx].remote_addr, CMQ_NODE_ADDR_SIZE);
                    ep2[CMQ_NODE_ADDR_SIZE - 1] = '\0';
                    eport2 = pool->conns[idx].remote_port;
                    cmq_mutex_unlock(&pool->io_locks[idx]);
                    if (alive && route_ep_same(ep2, eport2, addr, port)) {
                        cmq_mutex_unlock(&pool->lock);
                        close(fd);
                        return 0;
                    }
                    if (!route_fd_alive(efd) ||
                        (ep2[0] != '\0' &&
                         !route_ep_same(ep2, eport2, addr, port)))
                        route_slot_close(pool, idx);
                }
            }
            /* Rescan — peer may have been replaced while unlocked. */
            i = (size_t)-1;
            continue;
        }
        route_slot_close(pool, i);
        route_slot_install(pool, i, node_id, fd, 1, 1, addr, port);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    /* Reuse empty / fd<0 tombstone before growing (not staged fd>=0). */
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (efd >= 0)
            continue; /* live or staged */
        if (rid[0] != '\0' && connected)
            continue; /* connected+fd<0 placeholder */
        route_slot_close(pool, i);
        route_slot_install(pool, i, node_id, fd, 1, 1, addr, port);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        close(fd);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, 1, 1, addr, port);
    cmq_mutex_unlock(&pool->lock);
    return 0;
}

int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd,
                        const char *auth_user, const char *auth_pass) {
    if (!pool || !node_id) return -1;

    /* Reject pool-full before handshake so caller-owned fds are closed cleanly
       without blocking on a peer that will never be retained. */
    cmq_mutex_lock(&pool->lock);
    int replace = -1;
    int dead = -1;
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (strcmp(rid, node_id) == 0) {
            replace = (int)i;
            break;
        }
        if (dead < 0 && (rid[0] == '\0' || (!connected && efd < 0)))
            dead = (int)i;
    }
    if (replace < 0 && dead < 0 && pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        if (fd >= 0) close(fd);
        return -1;
    }
    cmq_mutex_unlock(&pool->lock);

    if (fd >= 0) {
        if (cmq_peer_handshake(fd, auth_user, auth_pass, CMQ_FLAG_ROUTE) != 0) {
            close(fd);
            return -1;
        }
        set_nonblock(fd);
    }

    cmq_mutex_lock(&pool->lock);
    if (replace >= 0 && (size_t)replace < pool->conn_count) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, (size_t)replace, &connected, &efd, NULL, rid);
        if (strcmp(rid, node_id) == 0) {
            if (efd >= 0 && efd != fd) {
                size_t idx = (size_t)replace;
                if (!connected) {
                    /* Staged inbound — do not SHUT_RDWR / steal. */
                    cmq_mutex_unlock(&pool->lock);
                    if (fd >= 0) close(fd);
                    return 0;
                }
                cmq_mutex_unlock(&pool->lock);
                int alive = route_fd_alive(efd);
                cmq_mutex_lock(&pool->lock);
                if (idx < pool->conn_count) {
                    int f2 = -1;
                    char rid2[CMQ_NODE_ID_SIZE];
                    route_slot_snap(pool, idx, NULL, &f2, NULL, rid2);
                    if (alive && strcmp(rid2, node_id) == 0 && f2 == efd) {
                        cmq_mutex_unlock(&pool->lock);
                        if (fd >= 0) close(fd);
                        return 0;
                    }
                    if (strcmp(rid2, node_id) == 0 && f2 == efd &&
                        !route_fd_alive(efd)) {
                        route_slot_close(pool, idx);
                        route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
                        cmq_mutex_unlock(&pool->lock);
                        return 0;
                    }
                    if (strcmp(rid2, node_id) == 0 && f2 >= 0) {
                        /* Live or staged peer appeared — keep it. */
                        cmq_mutex_unlock(&pool->lock);
                        if (fd >= 0) close(fd);
                        return 0;
                    }
                    if (strcmp(rid2, node_id) == 0) {
                        route_slot_close(pool, idx);
                        route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
                        cmq_mutex_unlock(&pool->lock);
                        return 0;
                    }
                }
                /* Fall through to rescan / grow. */
            } else {
                route_slot_install(pool, (size_t)replace, node_id, fd, fd >= 0, 1, NULL, 0);
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
        }
    }
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (strcmp(rid, node_id) != 0) continue;
        if (efd >= 0 && efd != fd) {
            size_t idx = i;
            if (!connected) {
                cmq_mutex_unlock(&pool->lock);
                if (fd >= 0) close(fd);
                return 0;
            }
            cmq_mutex_unlock(&pool->lock);
            int alive = route_fd_alive(efd);
            cmq_mutex_lock(&pool->lock);
            if (idx >= pool->conn_count) {
                i = (size_t)-1;
                continue;
            }
            int f2 = -1;
            char rid2[CMQ_NODE_ID_SIZE];
            route_slot_snap(pool, idx, NULL, &f2, NULL, rid2);
            if (strcmp(rid2, node_id) != 0) {
                i = (size_t)-1;
                continue;
            }
            if (alive && f2 == efd) {
                cmq_mutex_unlock(&pool->lock);
                if (fd >= 0) close(fd);
                return 0;
            }
            if (f2 == efd && !route_fd_alive(efd)) {
                route_slot_close(pool, idx);
                route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
            if (f2 >= 0) {
                cmq_mutex_unlock(&pool->lock);
                if (fd >= 0) close(fd);
                return 0;
            }
            route_slot_close(pool, idx);
            route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
        route_slot_install(pool, i, node_id, fd, fd >= 0, 1, NULL, 0);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (efd >= 0)
            continue; /* live or staged */
        if (rid[0] != '\0' && connected)
            continue; /* connected+fd<0 placeholder */
        route_slot_close(pool, i);
        route_slot_install(pool, i, node_id, fd, fd >= 0, 1, NULL, 0);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        if (fd >= 0) close(fd);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
    cmq_mutex_unlock(&pool->lock);
    return 0;
}

int cmq_route_attach_inbound(cmq_route_pool_t *pool, const char *node_id, int fd) {
    if (!pool || !node_id || fd < 0) return -1;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (strcmp(rid, node_id) != 0) continue;
        /* Existing fd for this node (live or staged): reject unless sticky dead. */
        if (efd >= 0) {
            size_t idx = i;
            if (!connected) {
                /* Staged inbound awaiting mark_connected — do not replace. */
                cmq_mutex_unlock(&pool->lock);
                return -1;
            }
            cmq_mutex_unlock(&pool->lock);
            int alive = route_fd_alive(efd);
            cmq_mutex_lock(&pool->lock);
            if (idx >= pool->conn_count) {
                i = (size_t)-1;
                continue;
            }
            char rid2[CMQ_NODE_ID_SIZE];
            int f2 = -1;
            route_slot_snap(pool, idx, NULL, &f2, NULL, rid2);
            if (strcmp(rid2, node_id) != 0) {
                i = (size_t)-1;
                continue;
            }
            if (f2 == efd) {
                /* Probe+identity: recycled fd must not reject attach forever. */
                if (alive) {
                    cmq_mutex_unlock(&pool->lock);
                    return -1;
                }
                if (!route_fd_alive(efd)) {
                    route_slot_close(pool, idx);
                    route_slot_install(pool, idx, node_id, fd, 0, 0, NULL, 0);
                    cmq_mutex_unlock(&pool->lock);
                    return 0;
                }
                cmq_mutex_unlock(&pool->lock);
                return -1;
            }
            if (f2 >= 0) {
                /* Live or staged peer appeared — do not SHUT_RDWR/steal. */
                cmq_mutex_unlock(&pool->lock);
                return -1;
            }
            route_slot_close(pool, idx);
            route_slot_install(pool, idx, node_id, fd, 0, 0, NULL, 0);
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
        route_slot_close(pool, i);
        route_slot_install(pool, i, node_id, fd, 0, 0, NULL, 0);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, i, &connected, &efd, NULL, rid);
        if (efd >= 0)
            continue; /* live or staged */
        if (rid[0] != '\0' && connected)
            continue; /* connected+fd<0 placeholder */
        route_slot_close(pool, i);
        route_slot_install(pool, i, node_id, fd, 0, 0, NULL, 0);
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, 0, 0, NULL, 0);
    cmq_mutex_unlock(&pool->lock);
    return 0;
}

int cmq_route_mark_connected(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return -1;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, NULL);
        if (efd != fd) continue;
        cmq_mutex_lock(&pool->io_locks[i]);
        int ok = 0;
        if (pool->conns[i].fd == fd) {
            pool->conns[i].connected = 1;
            ok = 1;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        cmq_mutex_unlock(&pool->lock);
        return ok ? 0 : -1;
    }
    cmq_mutex_unlock(&pool->lock);
    return -1;
}

void cmq_route_detach_fd(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, NULL);
        if (efd != fd) continue;
        /* pool->lock → io_lock (never reverse — avoids AB-BA with teardown). */
        cmq_mutex_lock(&pool->io_locks[i]);
        if (pool->conns[i].fd == fd) {
            pool->conns[i].fd = -1;
            pool->conns[i].connected = 0;
            pool->conns[i].fd_owned = 0;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        break;
    }
    cmq_mutex_unlock(&pool->lock);
}

int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            /* Tombstone in place — never memmove (io_locks are index-stable). */
            route_slot_close(pool, i);
            while (pool->conn_count > 0) {
                cmq_route_conn_t *last = &pool->conns[pool->conn_count - 1];
                if (last->remote_id[0] != '\0' || last->connected || last->fd >= 0)
                    break;
                pool->conn_count--;
            }
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
    size_t eagain = 0;
    size_t sent = cmq_route_broadcast(pool, data, len, exclude_id, &eagain);
    (void)eagain;
    return sent > 0 ? 0 : -1;
}

/* Returns peers written. If out_eagain non-NULL, set to peers skipped on EAGAIN. */
size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id,
                             size_t *out_eagain) {
    if (out_eagain) *out_eagain = 0;
    if (!pool || !data || len == 0) return 0;
    int fds[CMQ_ROUTE_MAX_CONNS];
    size_t idxs[CMQ_ROUTE_MAX_CONNS];
    char ids[CMQ_ROUTE_MAX_CONNS][CMQ_NODE_ID_SIZE];
    size_t n = 0;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    /* Read fd/connected/identity under io_lock (same as write-fail clears). */
    for (size_t i = 0; i < nslots && n < CMQ_ROUTE_MAX_CONNS; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        cmq_route_conn_t *c = &pool->conns[i];
        if (c->connected && c->fd >= 0 &&
            !(exclude_id && strcmp(c->remote_id, exclude_id) == 0)) {
            fds[n] = c->fd;
            idxs[n] = i;
            memcpy(ids[n], c->remote_id, CMQ_NODE_ID_SIZE);
            n++;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
    }

    size_t sent = 0;
    size_t deferred = 0;
    for (size_t j = 0; j < n; j++) {
        size_t idx = idxs[j];
        int expect_fd = fds[j];
        cmq_mutex_lock(&pool->io_locks[idx]);
        /* Validate under io_lock only — never nest pool->lock here (AB-BA with
           connect/disconnect holding pool->lock then taking io_lock). */
        int fd = -1;
        if (idx < CMQ_ROUTE_MAX_CONNS &&
            pool->conns[idx].connected &&
            pool->conns[idx].fd == expect_fd &&
            memcmp(pool->conns[idx].remote_id, ids[j], CMQ_NODE_ID_SIZE) == 0) {
            fd = expect_fd;
        }
        if (fd < 0) {
            cmq_mutex_unlock(&pool->io_locks[idx]);
            continue;
        }
        int wr = write_full(fd, data, len);
        if (wr == 0) {
            if (pool->conns[idx].fd == fd) {
                pool->conns[idx].bytes_sent += (uint64_t)len;
                pool->conns[idx].msgs_sent++;
            }
            cmq_mutex_unlock(&pool->io_locks[idx]);
            sent++;
        } else if (wr == 1) {
            cmq_mutex_unlock(&pool->io_locks[idx]);
            deferred++;
        } else {
            /* Drop under io_lock so other writers cannot race close/shutdown. */
            if (pool->conns[idx].fd == fd)
                conn_drop_fd(&pool->conns[idx]);
            cmq_mutex_unlock(&pool->io_locks[idx]);
        }
    }
    if (out_eagain) *out_eagain = deferred;
    return sent;
}

size_t cmq_route_pool_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    cmq_mutex_lock(&pool->lock);
    size_t c = 0;
    for (size_t i = 0; i < pool->conn_count; i++) {
        if (pool->conns[i].remote_id[0] != '\0')
            c++;
    }
    cmq_mutex_unlock(&pool->lock);
    return c;
}

size_t cmq_route_live_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    size_t n = 0;
    for (size_t i = 0; i < nslots; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        cmq_mutex_lock(&pool->io_locks[i]);
        int fd = -1;
        int cand = (pool->conns[i].connected && pool->conns[i].fd >= 0);
        if (cand) {
            fd = pool->conns[i].fd;
            memcpy(rid, pool->conns[i].remote_id, CMQ_NODE_ID_SIZE);
            rid[CMQ_NODE_ID_SIZE - 1] = '\0';
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (!cand) continue;
        int alive = route_fd_alive(fd);
        cmq_mutex_lock(&pool->io_locks[i]);
        int still = (pool->conns[i].connected && pool->conns[i].fd == fd &&
                     strcmp(pool->conns[i].remote_id, rid) == 0);
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (alive && still) {
            n++;
            continue;
        }
        cmq_mutex_lock(&pool->lock);
        if (i < pool->conn_count) {
            char rid2[CMQ_NODE_ID_SIZE];
            int efd2 = -1;
            route_slot_snap(pool, i, NULL, &efd2, NULL, rid2);
            if (efd2 == fd && strcmp(rid2, rid) == 0 && !route_fd_alive(efd2))
                route_slot_close(pool, i);
        }
        cmq_mutex_unlock(&pool->lock);
    }
    return n;
}

size_t cmq_route_held_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    size_t n = 0;
    for (size_t i = 0; i < nslots; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        int held = (pool->conns[i].fd >= 0);
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (held) n++;
    }
    return n;
}

int cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id,
                        cmq_route_conn_t *out) {
    if (!pool || !node_id || !out) return -1;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    /* Copy under io_lock so fd/connected match write-path death updates. */
    for (size_t i = 0; i < nslots; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            *out = pool->conns[i];
            cmq_mutex_unlock(&pool->io_locks[i]);
            return 0;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
    }
    return -1;
}

int cmq_route_peer_live(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return 0;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < nslots; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        int match = (strcmp(pool->conns[i].remote_id, node_id) == 0);
        int fd = -1;
        int cand = 0;
        if (match) {
            /* Staged inbound (connected=0, fd>=0) must block reconnect. */
            cand = (pool->conns[i].fd >= 0);
            if (cand) fd = pool->conns[i].fd;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (!match) continue;
        if (!cand) return 0;
        int alive = route_fd_alive(fd);
        cmq_mutex_lock(&pool->io_locks[i]);
        int still = (strcmp(pool->conns[i].remote_id, node_id) == 0 &&
                     pool->conns[i].fd == fd);
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (alive && still)
            return 1;
        cmq_mutex_lock(&pool->lock);
        if (i < pool->conn_count) {
            char rid2[CMQ_NODE_ID_SIZE];
            int efd2 = -1;
            route_slot_snap(pool, i, NULL, &efd2, NULL, rid2);
            if (strcmp(rid2, node_id) == 0 && efd2 == fd &&
                !route_fd_alive(efd2))
                route_slot_close(pool, i);
        }
        cmq_mutex_unlock(&pool->lock);
        return 0;
    }
    return 0;
}

int cmq_route_io_lock_fd(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return -1;
    cmq_mutex_lock(&pool->lock);
    int idx = -1;
    for (size_t i = 0; i < pool->conn_count; i++) {
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, NULL);
        if (efd == fd) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    /* pool→io (never reverse). Hold pool until io_lock so close+reconnect
       cannot reuse the same fd number into another slot mid-hand-off. */
    cmq_mutex_lock(&pool->io_locks[idx]);
    if ((size_t)idx >= pool->conn_count || pool->conns[idx].fd != fd) {
        cmq_mutex_unlock(&pool->io_locks[idx]);
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    cmq_mutex_unlock(&pool->lock);
    return idx;
}

void cmq_route_io_unlock_idx(cmq_route_pool_t *pool, int idx) {
    if (!pool || idx < 0 || (size_t)idx >= CMQ_ROUTE_MAX_CONNS) return;
    cmq_mutex_unlock(&pool->io_locks[idx]);
}
