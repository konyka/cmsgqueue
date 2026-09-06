#define _POSIX_C_SOURCE 200809L
#include "cmq_route.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_thread.h"
#include "cmq_types.h"
#include "cmq_route_tls_sess.h"

/* F17: TLS-aware read/write forward declarations. */
static ssize_t write_one(int fd, const uint8_t *data, size_t len,
                          cmq_route_tls_sess_t *sess);
static ssize_t read_one(int fd, void *buf, size_t len,
                         cmq_route_tls_sess_t *sess);

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
    /* Align CONNECT/config/leaf/gw: wire caps at 255 — never silently truncate. */
    if (auth_user) {
        size_t n = strlen(auth_user);
        if (n > 255) return -1;
        ulen = (uint16_t)n;
    }
    if (auth_pass) {
        size_t n = strlen(auth_pass);
        if (n > 255) return -1;
        pwen = (uint16_t)n;
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
        ssize_t n = write_one(fd, buf + off, len - off, NULL);
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
            /* Guard size_t wrap before need = HDR + plen (esp. 32-bit). */
            if ((size_t)plen_f > sizeof(rbuf) - sizeof(cmq_frame_hdr_t))
                return -1;
            size_t need = sizeof(cmq_frame_hdr_t) + (size_t)plen_f;
            if (rlen < need) break;

            if (op == (uint8_t)CMQ_OP_CONNACK) {
                if (plen_f < 1) return -1;
                if (rbuf[sizeof(cmq_frame_hdr_t)] != 0) return -1;
                /* Same read may already hold post-CONNACK frames (peer marked
                   connected and broadcast). Dropping them desyncs the stream. */
                if (rlen > need) return -1;
                return 0;
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
        ssize_t n = read_one(fd, rbuf + rlen, sizeof(rbuf) - rlen, NULL);
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

/* F17: TLS-aware read/write. When sess is NULL, fall back to plain
 * read/write. When sess is non-NULL, the bytes flow through
 * SSL_read/SSL_write with EAGAIN mapping for WANT_READ/WANT_WRITE. */
static ssize_t write_one(int fd, const uint8_t *data, size_t len,
                          cmq_route_tls_sess_t *sess) {
    if (sess) return cmq_route_tls_sess_write(sess, fd, data, len);
    ssize_t n;
    do {
        n = write(fd, data, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

static ssize_t read_one(int fd, void *buf, size_t len,
                         cmq_route_tls_sess_t *sess) {
    if (sess) return cmq_route_tls_sess_read(sess, fd, buf, len);
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

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

/* Desired outbound endpoint per node_id (gateway clusters[] analogue). */
typedef struct {
    char node_id[CMQ_NODE_ID_SIZE];
    char addr[CMQ_NODE_ADDR_SIZE];
    int port;
    int dialing; /* 1 while unlocked TCP+handshake in progress */
} cmq_route_target_t;

/* Per-node cancel token — independent of targets[] so add_conn/disconnect
   work without inflating target_count (forward_missed). */
typedef struct {
    char node_id[CMQ_NODE_ID_SIZE];
    uint32_t gen;
} cmq_route_cancel_t;

struct cmq_route_pool {
    cmq_cluster_t *cluster;
    cmq_route_conn_t conns[CMQ_ROUTE_MAX_CONNS];
    size_t conn_count;
    cmq_route_target_t targets[CMQ_ROUTE_MAX_CONNS];
    size_t target_count;
    cmq_route_cancel_t cancels[CMQ_ROUTE_MAX_CONNS];
    size_t cancel_count;
    uint32_t cancel_epoch; /* bumps when cancels[] full — foil snap=0 dials */
    cmq_route_interest_t interests[256];
    size_t interest_count;
    cmq_mutex_t lock;
    cmq_mutex_t io_locks[CMQ_ROUTE_MAX_CONNS]; /* per-slot write serialization */
    atomic_int in_flight; /* connect/add_conn unlocked dial/handshake */
    atomic_int dying;
    cmq_atomic_int *dial_gate; /* optional; non-zero aborts post-dial install */
};


static int route_begin_op(cmq_route_pool_t *pool) {
    if (atomic_load_explicit(&pool->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&pool->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&pool->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void route_end_op(cmq_route_pool_t *pool) {
    atomic_fetch_sub_explicit(&pool->in_flight, 1, memory_order_acq_rel);
}

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

/* Zero-timeout TCP liveness — detect peer FIN (poll events=0 misses it). */
int cmq_tcp_fd_alive(int fd) {
    if (fd < 0) return 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLERR | POLLHUP };
    int pr = poll(&pfd, 1, 0);
    if (pr < 0)
        return errno == EINTR;
    if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return 0;
    if (pr > 0 && (pfd.revents & POLLIN)) {
        char b;
        ssize_t n = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return 0;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return 0;
    }
    return 1;
}

static int route_fd_alive(int fd) {
    return cmq_tcp_fd_alive(fd);
}

/* Empty remote_addr = inbound/placeholder (no configured endpoint). */
static int route_ep_same(const char *slot_addr, int slot_port,
                          const char *addr, int port) {
    if (!slot_addr || slot_addr[0] == '\0')
        return 1;
    return slot_port == port &&
           strncmp(slot_addr, addr, CMQ_NODE_ADDR_SIZE) == 0;
}

/* Caller holds pool->lock. */
static ssize_t route_find_target(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) == 0)
            return (ssize_t)i;
    }
    return -1;
}

static ssize_t route_find_cancel(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    for (size_t i = 0; i < pool->cancel_count; i++) {
        if (strcmp(pool->cancels[i].node_id, node_id) == 0)
            return (ssize_t)i;
    }
    return -1;
}

/* Caller holds pool->lock. Allow immediate reconnect after cancel. */
static void route_clear_target_dialing(cmq_route_pool_t *pool,
                                       const char *node_id) {
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) == 0) {
            pool->targets[i].dialing = 0;
            return;
        }
    }
}

/* Bump (or create) per-node cancel gen. Caller holds pool->lock. */
static void route_bump_cancel(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return;
    ssize_t i = route_find_cancel(pool, node_id);
    if (i >= 0) {
        pool->cancels[i].gen++;
        /* Old dial still fails install on gen; clear so connect can redial now. */
        route_clear_target_dialing(pool, node_id);
        return;
    }
    if (pool->cancel_count >= CMQ_ROUTE_MAX_CONNS) {
        /* Table full — bump gens + epoch so snap=0 (untracked) dials fail closed. */
        for (size_t j = 0; j < pool->cancel_count; j++)
            pool->cancels[j].gen++;
        pool->cancel_epoch++;
        for (size_t j = 0; j < pool->target_count; j++)
            pool->targets[j].dialing = 0;
        return;
    }
    cmq_route_cancel_t *c = &pool->cancels[pool->cancel_count++];
    snprintf(c->node_id, sizeof(c->node_id), "%s", node_id);
    c->gen = 1;
    route_clear_target_dialing(pool, node_id);
}

/* Caller holds pool->lock. */
static void route_cancel_snap(cmq_route_pool_t *pool, const char *node_id,
                               uint32_t *gen_out, uint32_t *epoch_out) {
    ssize_t i = route_find_cancel(pool, node_id);
    if (gen_out)
        *gen_out = i >= 0 ? pool->cancels[i].gen : 0;
    if (epoch_out)
        *epoch_out = pool->cancel_epoch;
}

static int route_cancel_changed(cmq_route_pool_t *pool, const char *node_id,
                                 uint32_t gen, uint32_t epoch) {
    if (pool->cancel_epoch != epoch)
        return 1;
    ssize_t i = route_find_cancel(pool, node_id);
    if (i < 0)
        return gen != 0; /* entry removed — treat as cancelled if we had a snap */
    return pool->cancels[i].gen != gen;
}

/* Caller holds pool->lock. True if node_id still maps to addr:port
   (connect may have rewritten the target while dial was unlocked). */
static int route_endpoint_current(cmq_route_pool_t *pool, const char *node_id,
                                  const char *addr, int port) {
    if (!pool || !node_id || !addr) return 0;
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) == 0)
            return pool->targets[i].port == port &&
                   strncmp(pool->targets[i].addr, addr, CMQ_NODE_ADDR_SIZE) == 0;
    }
    return 0;
}

/* Caller holds pool->lock. Publish desired endpoint; on change, drop all
   peers for node_id (including inbound empty-addr) so connect can dial the
   new addr — otherwise sticky inbound blocks outbound forever. */
static int route_set_target(cmq_route_pool_t *pool, const char *node_id,
                            const char *addr, int port) {
    if (!pool || !node_id || !addr) return -1;
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) != 0) continue;
        int changed = (pool->targets[i].port != port) ||
                      (strcmp(pool->targets[i].addr, addr) != 0);
        snprintf(pool->targets[i].addr, sizeof(pool->targets[i].addr),
                 "%s", addr);
        pool->targets[i].port = port;
        if (changed) {
            /* Cancel in-flight dials to the old endpoint. */
            route_bump_cancel(pool, node_id);
            for (size_t j = 0; j < pool->conn_count; j++) {
                cmq_mutex_lock(&pool->io_locks[j]);
                int match = (strcmp(pool->conns[j].remote_id, node_id) == 0);
                cmq_mutex_unlock(&pool->io_locks[j]);
                if (match)
                    route_slot_close(pool, j);
            }
        }
        return 0;
    }
    if (pool->target_count >= CMQ_ROUTE_MAX_CONNS) return -1;
    cmq_route_target_t *t = &pool->targets[pool->target_count++];
    snprintf(t->node_id, sizeof(t->node_id), "%s", node_id);
    snprintf(t->addr, sizeof(t->addr), "%s", addr);
    t->port = port;
    t->dialing = 0;
    return 0;
}

/* Caller holds pool->lock. Returns 0 and sets dialing, or -1 if already dialing. */
static int route_target_dial_begin(cmq_route_pool_t *pool, const char *node_id) {
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) != 0) continue;
        if (pool->targets[i].dialing) return -1;
        pool->targets[i].dialing = 1;
        return 0;
    }
    return -1;
}

static void route_target_dial_end(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->target_count; i++) {
        if (strcmp(pool->targets[i].node_id, node_id) == 0) {
            pool->targets[i].dialing = 0;
            break;
        }
    }
    cmq_mutex_unlock(&pool->lock);
}

/* Publish a peer into a slot. Caller holds pool->lock.
   connected=0 stages inbound until handshake is drained.
   addr may be NULL for inbound/placeholder (no endpoint sticky). */
static void route_slot_install(cmq_route_pool_t *pool, size_t idx,
                                const char *node_id, int fd, int fd_owned,
                                int connected, const char *addr, int port) {
    cmq_mutex_lock(&pool->io_locks[idx]);
    memset(&pool->conns[idx], 0, sizeof(pool->conns[idx]));
    snprintf(pool->conns[idx].remote_id, sizeof(pool->conns[idx].remote_id),
             "%s", node_id);
    if (addr && addr[0]) {
        snprintf(pool->conns[idx].remote_addr,
                 sizeof(pool->conns[idx].remote_addr), "%s", addr);
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
    p->dial_gate = NULL;
    atomic_init(&p->in_flight, 0);
    atomic_init(&p->dying, 0);
    cmq_mutex_init(&p->lock);
    for (size_t i = 0; i < CMQ_ROUTE_MAX_CONNS; i++)
        cmq_mutex_init(&p->io_locks[i]);
    return p;
}

void cmq_route_pool_set_dial_gate(cmq_route_pool_t *pool, cmq_atomic_int *gate) {
    if (!pool) return;
    if (route_begin_op(pool) != 0) return;
    pool->dial_gate = gate;
    route_end_op(pool);
}

/* 1 if server drain (or similar) forbids installing a freshly dialed peer. */
static int route_dial_gated(const cmq_route_pool_t *pool) {
    if (!pool || !pool->dial_gate) return 0;
    return cmq_atomic_load_int(pool->dial_gate, CMQ_ATOMIC_ACQUIRE) != 0;
}

void cmq_route_pool_destroy(cmq_route_pool_t *pool) {
    if (!pool) return;
    atomic_store_explicit(&pool->dying, 1, memory_order_release);
    while (atomic_load_explicit(&pool->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++)
        route_slot_close(pool, i);
    cmq_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < CMQ_ROUTE_MAX_CONNS; i++)
        cmq_mutex_destroy(&pool->io_locks[i]);
    cmq_mutex_destroy(&pool->lock);
    free(pool);
}

static int route_connect_impl(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass) {
    if (!pool || !node_id || !addr) return -1;
    if (strnlen(node_id, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE) return -1;
    if (strnlen(addr, CMQ_NODE_ADDR_SIZE) >= CMQ_NODE_ADDR_SIZE) return -1;
    if (port <= 0 || port > 65535) return -1;

    cmq_mutex_lock(&pool->lock);
    if (route_set_target(pool, node_id, addr, port) != 0) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        int match = (strcmp(pool->conns[i].remote_id, node_id) == 0);
        int efd = pool->conns[i].fd;
        cmq_mutex_unlock(&pool->io_locks[i]);
        /* Live or staged (fd>=0). Sticky only for same configured endpoint;
           inbound/empty addr keeps any alive peer (do not steal staged). */
        if (match && efd >= 0) {
            size_t idx = i;
            cmq_mutex_lock(&pool->io_locks[idx]);
            int same = (strcmp(pool->conns[idx].remote_id, node_id) == 0 &&
                        pool->conns[idx].fd == efd);
            char ep2[CMQ_NODE_ADDR_SIZE];
            int eport2 = 0;
            int live = 0;
            int reclaim = 0;
            if (same) {
                memcpy(ep2, pool->conns[idx].remote_addr, CMQ_NODE_ADDR_SIZE);
                ep2[CMQ_NODE_ADDR_SIZE - 1] = '\0';
                eport2 = pool->conns[idx].remote_port;
                /* Probe under io_lock — do not re-probe bare efd after unlock. */
                int ep_ok = route_ep_same(ep2, eport2, addr, port);
                int alive = route_fd_alive(efd);
                live = ep_ok && alive;
                reclaim = !alive ||
                          (ep2[0] != '\0' && !ep_ok);
            }
            cmq_mutex_unlock(&pool->io_locks[idx]);
            if (live) {
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
            /* Drop dead or wrong-endpoint outbound; never steal inbound. */
            if (same && reclaim)
                route_slot_close(pool, idx);
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
        /* Also reclaim sticky connected=1 slots whose TCP is already dead. */
        for (size_t i = 0; !usable && i < pool->conn_count; ) {
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, NULL);
            if (!connected || efd < 0) {
                usable = 1;
                break;
            }
            cmq_mutex_lock(&pool->io_locks[i]);
            int still = (pool->conns[i].fd == efd && pool->conns[i].connected);
            int live = still && route_fd_alive(efd);
            cmq_mutex_unlock(&pool->io_locks[i]);
            if (!still) {
                i = 0;
                continue;
            }
            if (!live) {
                route_slot_close(pool, i);
                usable = 1;
                break;
            }
            i++;
        }
        if (!usable) {
            cmq_mutex_unlock(&pool->lock);
            return -1;
        }
    }
    uint32_t cgen = 0, cepoch = 0;
    route_cancel_snap(pool, node_id, &cgen, &cepoch);
    /* Serialize unlocked dial — reconnect overlaps connect+handshake (~5s). */
    if (route_target_dial_begin(pool, node_id) != 0) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    cmq_mutex_unlock(&pool->lock);

    /* Connect + handshake outside the pool lock so broadcast can proceed. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        route_target_dial_end(pool, node_id);
        return -1;
    }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }
    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_ROUTE_CONNECT_MS) != 0) {
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }
    if (cmq_peer_handshake(fd, auth_user, auth_pass, CMQ_FLAG_ROUTE) != 0) {
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }
    set_nonblock(fd);

    /* Drain may have started during unlocked dial — do not install egress. */
    if (route_dial_gated(pool)) {
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }

    cmq_mutex_lock(&pool->lock);
    /* disconnect / endpoint change / drain during unlocked dial. */
    if (route_cancel_changed(pool, node_id, cgen, cepoch) ||
        !route_endpoint_current(pool, node_id, addr, port) ||
        route_dial_gated(pool)) {
        cmq_mutex_unlock(&pool->lock);
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, rid);
        if (strcmp(rid, node_id) != 0) continue;
        /* Another thread may have restored a live or staged egress. */
        if (efd >= 0) {
            size_t idx = i;
            if (!route_endpoint_current(pool, node_id, addr, port)) {
                cmq_mutex_unlock(&pool->lock);
                close(fd);
                route_target_dial_end(pool, node_id);
                return -1;
            }
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
                    /* Probe under io_lock — do not discard a fresh dial on
                       unlock-then-EINTR false-alive. */
                    int ep_ok = route_ep_same(ep2, eport2, addr, port);
                    int alive = route_fd_alive(efd);
                    int live = ep_ok && alive;
                    int reclaim = !alive ||
                                  (ep2[0] != '\0' && !ep_ok);
                    cmq_mutex_unlock(&pool->io_locks[idx]);
                    if (live) {
                        cmq_mutex_unlock(&pool->lock);
                        close(fd);
                        route_target_dial_end(pool, node_id);
                        return 0;
                    }
                    /* Dead / wrong-ep: reclaim from locked result only. */
                    if (reclaim)
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
        route_target_dial_end(pool, node_id);
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
        route_target_dial_end(pool, node_id);
        return 0;
    }
    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        /* Post-handshake: reclaim sticky dead TCP before failing. */
        for (size_t i = 0; i < pool->conn_count; ) {
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, NULL);
            if (!connected || efd < 0) {
                i++;
                continue;
            }
            if (!route_endpoint_current(pool, node_id, addr, port)) {
                cmq_mutex_unlock(&pool->lock);
                close(fd);
                route_target_dial_end(pool, node_id);
                return -1;
            }
            cmq_mutex_lock(&pool->io_locks[i]);
            int still = (pool->conns[i].fd == efd && pool->conns[i].connected);
            int live = still && route_fd_alive(efd);
            cmq_mutex_unlock(&pool->io_locks[i]);
            if (!still) {
                i = 0;
                continue;
            }
            if (!live) {
                route_slot_close(pool, i);
                route_slot_install(pool, i, node_id, fd, 1, 1, addr, port);
                cmq_mutex_unlock(&pool->lock);
                route_target_dial_end(pool, node_id);
                return 0;
            }
            i++;
        }
        cmq_mutex_unlock(&pool->lock);
        close(fd);
        route_target_dial_end(pool, node_id);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, 1, 1, addr, port);
    cmq_mutex_unlock(&pool->lock);
    route_target_dial_end(pool, node_id);
    return 0;
}

static int route_add_conn_impl(cmq_route_pool_t *pool, const char *node_id, int fd,
                        const char *auth_user, const char *auth_pass) {
    if (!pool || !node_id) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (strnlen(node_id, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (route_dial_gated(pool)) {
        if (fd >= 0) close(fd);
        return -1;
    }

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
        /* Reclaim sticky connected=1 dead TCP before failing pool-full. */
        for (size_t i = 0; i < pool->conn_count; ) {
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, NULL);
            if (!connected || efd < 0) {
                dead = (int)i;
                break;
            }
            cmq_mutex_lock(&pool->io_locks[i]);
            int still = (pool->conns[i].fd == efd && pool->conns[i].connected);
            int live = still && route_fd_alive(efd);
            cmq_mutex_unlock(&pool->io_locks[i]);
            if (!still) {
                i = 0;
                continue;
            }
            if (!live) {
                route_slot_close(pool, i);
                dead = (int)i;
                break;
            }
            i++;
        }
        if (replace < 0 && dead < 0) {
            cmq_mutex_unlock(&pool->lock);
            if (fd >= 0) close(fd);
            return -1;
        }
    }
    uint32_t cgen = 0, cepoch = 0;
    route_cancel_snap(pool, node_id, &cgen, &cepoch);
    cmq_mutex_unlock(&pool->lock);

    if (fd >= 0) {
        if (cmq_peer_handshake(fd, auth_user, auth_pass, CMQ_FLAG_ROUTE) != 0) {
            close(fd);
            return -1;
        }
        set_nonblock(fd);
    }
    if (route_dial_gated(pool)) {
        if (fd >= 0) close(fd);
        return -1;
    }

    cmq_mutex_lock(&pool->lock);
    /* Align connect: disconnect during unlocked handshake must not install. */
    if (route_cancel_changed(pool, node_id, cgen, cepoch) || route_dial_gated(pool)) {
        cmq_mutex_unlock(&pool->lock);
        if (fd >= 0) close(fd);
        return -1;
    }
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
                if (idx < pool->conn_count) {
                    int f2 = -1;
                    char rid2[CMQ_NODE_ID_SIZE];
                    route_slot_snap(pool, idx, NULL, &f2, NULL, rid2);
                    if (strcmp(rid2, node_id) == 0 && f2 == efd) {
                        cmq_mutex_lock(&pool->io_locks[idx]);
                        int still = (pool->conns[idx].fd == efd);
                        int live = still && route_fd_alive(efd);
                        int reclaim = still && !live;
                        cmq_mutex_unlock(&pool->io_locks[idx]);
                        if (live) {
                            cmq_mutex_unlock(&pool->lock);
                            if (fd >= 0) close(fd);
                            return 0;
                        }
                        if (reclaim) {
                            route_slot_close(pool, idx);
                            route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
                            cmq_mutex_unlock(&pool->lock);
                            return 0;
                        }
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
            if (f2 == efd) {
                cmq_mutex_lock(&pool->io_locks[idx]);
                int still = (pool->conns[idx].fd == efd);
                int live = still && route_fd_alive(efd);
                int reclaim = still && !live;
                cmq_mutex_unlock(&pool->io_locks[idx]);
                if (live) {
                    cmq_mutex_unlock(&pool->lock);
                    if (fd >= 0) close(fd);
                    return 0;
                }
                if (reclaim) {
                    route_slot_close(pool, idx);
                    route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
                    cmq_mutex_unlock(&pool->lock);
                    return 0;
                }
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
        for (size_t i = 0; i < pool->conn_count; ) {
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, NULL);
            if (!connected || efd < 0) {
                i++;
                continue;
            }
            cmq_mutex_lock(&pool->io_locks[i]);
            int still = (pool->conns[i].fd == efd && pool->conns[i].connected);
            int live = still && route_fd_alive(efd);
            cmq_mutex_unlock(&pool->io_locks[i]);
            if (!still) {
                i = 0;
                continue;
            }
            if (!live) {
                route_slot_close(pool, i);
                route_slot_install(pool, i, node_id, fd, fd >= 0, 1, NULL, 0);
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
            i++;
        }
        cmq_mutex_unlock(&pool->lock);
        if (fd >= 0) close(fd);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, fd >= 0, 1, NULL, 0);
    cmq_mutex_unlock(&pool->lock);
    return 0;
}

static int route_attach_inbound_impl(cmq_route_pool_t *pool, const char *node_id, int fd) {
    if (!pool || !node_id || fd < 0) return -1;
    if (strnlen(node_id, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE) return -1;
    /* Borrowed fd — do not close on gate; caller owns the socket. */
    if (route_dial_gated(pool)) return -1;
    cmq_mutex_lock(&pool->lock);
    if (route_dial_gated(pool)) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
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
            /* Identity + live probe under pool/io locks (no unlock-stale alive). */
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
                cmq_mutex_lock(&pool->io_locks[idx]);
                int still = (pool->conns[idx].fd == efd);
                int live = still && route_fd_alive(efd);
                int reclaim = still && !live;
                cmq_mutex_unlock(&pool->io_locks[idx]);
                if (live) {
                    cmq_mutex_unlock(&pool->lock);
                    return -1;
                }
                if (reclaim) {
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
        /* Inbound attach: reclaim sticky dead outbound before rejecting. */
        for (size_t i = 0; i < pool->conn_count; ) {
            int connected = 0, efd = -1;
            route_slot_snap(pool, i, &connected, &efd, NULL, NULL);
            if (!connected || efd < 0) {
                i++;
                continue;
            }
            cmq_mutex_lock(&pool->io_locks[i]);
            int still = (pool->conns[i].fd == efd && pool->conns[i].connected);
            int live = still && route_fd_alive(efd);
            cmq_mutex_unlock(&pool->io_locks[i]);
            if (!still) {
                i = 0;
                continue;
            }
            if (!live) {
                route_slot_close(pool, i);
                route_slot_install(pool, i, node_id, fd, 0, 0, NULL, 0);
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
            i++;
        }
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    size_t idx = pool->conn_count++;
    route_slot_install(pool, idx, node_id, fd, 0, 0, NULL, 0);
    cmq_mutex_unlock(&pool->lock);
    return 0;
}

static int route_mark_connected_impl(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return -1;
    if (route_dial_gated(pool)) return -1;
    cmq_mutex_lock(&pool->lock);
    if (route_dial_gated(pool)) {
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
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

/* connected=0, keep fd/remote_id — flush and route_io_lock_fd still work. */
static void route_unmark_connected_fd_impl(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        int efd = -1;
        route_slot_snap(pool, i, NULL, &efd, NULL, NULL);
        if (efd != fd) continue;
        cmq_mutex_lock(&pool->io_locks[i]);
        if (pool->conns[i].fd == fd)
            pool->conns[i].connected = 0;
        cmq_mutex_unlock(&pool->io_locks[i]);
        break;
    }
    cmq_mutex_unlock(&pool->lock);
}

static void route_detach_fd_impl(cmq_route_pool_t *pool, int fd) {
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
            /* Align with inbound hard-fail — drop named tombstone so
               pool_count/forward_missed are not stuck until slot reuse. */
            pool->conns[i].remote_id[0] = '\0';
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        break;
    }
    cmq_mutex_unlock(&pool->lock);
}

static int route_disconnect_impl(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    cmq_mutex_lock(&pool->lock);
    /* Always bump — cancel in-flight add_conn even if slot/target absent
       (align leaf_remove; cancels[] exists for non-target handshake paths). */
    route_bump_cancel(pool, node_id);
    int known = (route_find_target(pool, node_id) >= 0);
    int closed = 0;
    /* Close every matching slot — dual inbound+outbound (or retry residue)
       must not leave a live peer after disconnect. */
    for (size_t i = 0; i < pool->conn_count; i++) {
        char rid[CMQ_NODE_ID_SIZE];
        route_slot_snap(pool, i, NULL, NULL, NULL, rid);
        if (strcmp(rid, node_id) == 0) {
            /* Tombstone in place — never memmove (io_locks are index-stable). */
            route_slot_close(pool, i);
            closed = 1;
        }
    }
    while (pool->conn_count > 0) {
        size_t last = pool->conn_count - 1;
        char lrid[CMQ_NODE_ID_SIZE];
        int connected = 0, efd = -1;
        route_slot_snap(pool, last, &connected, &efd, NULL, lrid);
        if (lrid[0] != '\0' || connected || efd >= 0)
            break;
        pool->conn_count--;
    }
    cmq_mutex_unlock(&pool->lock);
    return (closed || known) ? 0 : -1;
}

/* Bind-fail cleanup: drop only the remembered owned egress under pool→io.
   Snap-then-disconnect(nid) races fd recycle into the same nid. */
static int route_disconnect_if_owned_fd_impl(cmq_route_pool_t *pool,
                                             const char *node_id,
                                             int expect_fd) {
    if (!pool || !node_id || expect_fd < 0) return -1;
    cmq_mutex_lock(&pool->lock);
    int closed = 0;
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        int match = (strcmp(pool->conns[i].remote_id, node_id) == 0 &&
                     pool->conns[i].fd == expect_fd &&
                     pool->conns[i].fd_owned);
        if (match) {
            conn_drop_fd(&pool->conns[i]);
            memset(&pool->conns[i], 0, sizeof(pool->conns[i]));
            closed = 1;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (closed) break;
    }
    if (closed) {
        route_bump_cancel(pool, node_id);
        while (pool->conn_count > 0) {
            size_t last = pool->conn_count - 1;
            char lrid[CMQ_NODE_ID_SIZE];
            int connected = 0, efd = -1;
            route_slot_snap(pool, last, &connected, &efd, NULL, lrid);
            if (lrid[0] != '\0' || connected || efd >= 0)
                break;
            pool->conn_count--;
        }
    }
    cmq_mutex_unlock(&pool->lock);
    return closed ? 0 : -1;
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
    if (route_begin_op(pool) != 0) return 0;
    int fds[CMQ_ROUTE_MAX_CONNS];
    size_t idxs[CMQ_ROUTE_MAX_CONNS];
    char ids[CMQ_ROUTE_MAX_CONNS][CMQ_NODE_ID_SIZE];
    size_t n = 0;
    /* Snapshot under pool→io so conn_count cannot grow mid-scan (missed peers). */
    cmq_mutex_lock(&pool->lock);
    size_t nslots = pool->conn_count;
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
    cmq_mutex_unlock(&pool->lock);

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
            /* Snapshotted peer vanished — count undelivered so forward_op
               returns -1 (else sent=0,eagain=0 looks like success). */
            cmq_mutex_unlock(&pool->io_locks[idx]);
            deferred++;
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
            /* Drop under io_lock so other writers cannot race close/shutdown.
               Inbound (borrowed): SHUT_RDWR + fd=-1 + clear remote_id so the
               slot is reusable (keeping fd>=0 blocked attach_inbound forever).
               Owned: close. Count as undelivered for drop stats. */
            if (pool->conns[idx].fd == fd) {
                conn_drop_fd(&pool->conns[idx]);
                pool->conns[idx].remote_id[0] = '\0';
            }
            cmq_mutex_unlock(&pool->io_locks[idx]);
            deferred++;
        }
    }
    if (out_eagain) *out_eagain = deferred;
    route_end_op(pool);
    return sent;
}

static size_t route_pool_count_impl(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    cmq_mutex_lock(&pool->lock);
    size_t c = 0;
    for (size_t i = 0; i < pool->conn_count; i++) {
        /* Identity published/cleared under io_lock — match live_count/forward. */
        char rid[CMQ_NODE_ID_SIZE];
        route_slot_snap(pool, i, NULL, NULL, NULL, rid);
        if (rid[0] != '\0')
            c++;
    }
    cmq_mutex_unlock(&pool->lock);
    return c;
}

static size_t route_live_count_impl(cmq_route_pool_t *pool) {
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
        cmq_mutex_lock(&pool->io_locks[i]);
        int still = (pool->conns[i].connected && pool->conns[i].fd == fd &&
                     strcmp(pool->conns[i].remote_id, rid) == 0);
        /* Probe under io_lock — reclaim from locked result only. */
        int live = still && route_fd_alive(fd);
        int reclaim = still && !live;
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (live) {
            n++;
            continue;
        }
        if (!reclaim) continue;
        /* pool→io: re-probe before close — broadcast may have dropped the
           dead fd under io_lock only; reconnect can recycle the same fd# into
           this slot with the same rid. Snap-only match would kill the new peer. */
        cmq_mutex_lock(&pool->lock);
        if (i < pool->conn_count) {
            cmq_mutex_lock(&pool->io_locks[i]);
            if (pool->conns[i].fd == fd &&
                strcmp(pool->conns[i].remote_id, rid) == 0 &&
                !route_fd_alive(fd)) {
                conn_drop_fd(&pool->conns[i]);
                memset(&pool->conns[i], 0, sizeof(pool->conns[i]));
            }
            cmq_mutex_unlock(&pool->io_locks[i]);
        }
        cmq_mutex_unlock(&pool->lock);
    }
    return n;
}

static size_t route_held_count_impl(cmq_route_pool_t *pool) {
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

static int route_get_conn_impl(cmq_route_pool_t *pool, const char *node_id,
                        cmq_route_conn_t *out) {
    if (!pool || !node_id || !out) return -1;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    /* Copy under io_lock so fd/connected match write-path death updates. */
    for (size_t i = 0; i < nslots; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        if (strcmp(pool->conns[i].remote_id, node_id) != 0) {
            cmq_mutex_unlock(&pool->io_locks[i]);
            continue;
        }
        int fd = pool->conns[i].fd;
        int sticky = pool->conns[i].connected && fd >= 0;
        int dead = sticky && !route_fd_alive(fd);
        if (!dead) {
            *out = pool->conns[i];
            cmq_mutex_unlock(&pool->io_locks[i]);
            return 0;
        }
        cmq_mutex_unlock(&pool->io_locks[i]);
        /* Align peer_live: reclaim sticky-dead under pool→io, then miss. */
        cmq_mutex_lock(&pool->lock);
        if (i < pool->conn_count) {
            cmq_mutex_lock(&pool->io_locks[i]);
            if (pool->conns[i].fd == fd &&
                strcmp(pool->conns[i].remote_id, node_id) == 0 &&
                !route_fd_alive(fd)) {
                conn_drop_fd(&pool->conns[i]);
                memset(&pool->conns[i], 0, sizeof(pool->conns[i]));
            }
            cmq_mutex_unlock(&pool->io_locks[i]);
        }
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    return -1;
}

static int route_peer_live_impl(cmq_route_pool_t *pool, const char *node_id) {
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
        cmq_mutex_lock(&pool->io_locks[i]);
        int still = (strcmp(pool->conns[i].remote_id, node_id) == 0 &&
                     pool->conns[i].fd == fd);
        /* Re-probe under io_lock — reclaim from locked result only. */
        int live = still && route_fd_alive(fd);
        int reclaim = still && !live;
        cmq_mutex_unlock(&pool->io_locks[i]);
        if (live)
            return 1;
        if (reclaim) {
            /* Same fd-recycle TOCTOU as live_count — re-probe under pool→io. */
            cmq_mutex_lock(&pool->lock);
            if (i < pool->conn_count) {
                cmq_mutex_lock(&pool->io_locks[i]);
                if (pool->conns[i].fd == fd &&
                    strcmp(pool->conns[i].remote_id, node_id) == 0 &&
                    !route_fd_alive(fd)) {
                    conn_drop_fd(&pool->conns[i]);
                    memset(&pool->conns[i], 0, sizeof(pool->conns[i]));
                }
                cmq_mutex_unlock(&pool->io_locks[i]);
            }
            cmq_mutex_unlock(&pool->lock);
        }
        return 0;
    }
    return 0;
}

int cmq_route_io_lock_fd(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return -1;
    if (route_begin_op(pool) != 0) return -1;
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
        route_end_op(pool);
        return -1;
    }
    /* pool→io (never reverse). Hold pool until io_lock so close+reconnect
       cannot reuse the same fd number into another slot mid-hand-off. */
    cmq_mutex_lock(&pool->io_locks[idx]);
    if ((size_t)idx >= pool->conn_count || pool->conns[idx].fd != fd) {
        cmq_mutex_unlock(&pool->io_locks[idx]);
        cmq_mutex_unlock(&pool->lock);
        route_end_op(pool);
        return -1;
    }
    cmq_mutex_unlock(&pool->lock);
    return idx;
}

void cmq_route_io_unlock_idx(cmq_route_pool_t *pool, int idx) {
    if (!pool || idx < 0 || (size_t)idx >= CMQ_ROUTE_MAX_CONNS) return;
    cmq_mutex_unlock(&pool->io_locks[idx]);
    route_end_op(pool);
}



int cmq_route_attach_inbound(cmq_route_pool_t *pool, const char *node_id, int fd) {
    if (!pool || !node_id || fd < 0) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_attach_inbound_impl(pool, node_id, fd);
    route_end_op(pool);
    return rc;
}

int cmq_route_mark_connected(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_mark_connected_impl(pool, fd);
    route_end_op(pool);
    return rc;
}

void cmq_route_unmark_connected_fd(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return;
    if (route_begin_op(pool) != 0) return;
    route_unmark_connected_fd_impl(pool, fd);
    route_end_op(pool);
}

void cmq_route_detach_fd(cmq_route_pool_t *pool, int fd) {
    if (!pool || fd < 0) return;
    if (route_begin_op(pool) != 0) return;
    route_detach_fd_impl(pool, fd);
    route_end_op(pool);
}

static int route_adopt_fd_impl(cmq_route_pool_t *pool, int fd,
                                const char *remote_id) {
    if (!pool || fd < 0 || !remote_id || remote_id[0] == '\0') return -1;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        int efd = -1, owned = 0;
        char rid[CMQ_NODE_ID_SIZE];
        route_slot_snap(pool, i, NULL, &efd, &owned, rid);
        if (efd != fd || strcmp(rid, remote_id) != 0) continue;
        cmq_mutex_lock(&pool->io_locks[i]);
        /* CAS-style: only the first binder may claim an owned egress.
           Already-borrowed, replaced, or wrong-peer fd must fail. */
        int ok = (pool->conns[i].fd == fd && pool->conns[i].fd_owned &&
                  strcmp(pool->conns[i].remote_id, remote_id) == 0);
        if (ok)
            pool->conns[i].fd_owned = 0;
        cmq_mutex_unlock(&pool->io_locks[i]);
        cmq_mutex_unlock(&pool->lock);
        return ok ? 0 : -1;
    }
    cmq_mutex_unlock(&pool->lock);
    return -1;
}

int cmq_route_adopt_fd(cmq_route_pool_t *pool, int fd, const char *remote_id) {
    if (!pool || fd < 0 || !remote_id) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_adopt_fd_impl(pool, fd, remote_id);
    route_end_op(pool);
    return rc;
}

int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_disconnect_impl(pool, node_id);
    route_end_op(pool);
    return rc;
}

int cmq_route_disconnect_if_owned_fd(cmq_route_pool_t *pool, const char *node_id,
                                      int expect_fd) {
    if (!pool || !node_id || expect_fd < 0) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_disconnect_if_owned_fd_impl(pool, node_id, expect_fd);
    route_end_op(pool);
    return rc;
}

size_t cmq_route_pool_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    if (route_begin_op(pool) != 0) return 0;
    size_t c = route_pool_count_impl(pool);
    route_end_op(pool);
    return c;
}

size_t cmq_route_live_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    if (route_begin_op(pool) != 0) return 0;
    size_t c = route_live_count_impl(pool);
    route_end_op(pool);
    return c;
}

size_t cmq_route_held_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    if (route_begin_op(pool) != 0) return 0;
    size_t c = route_held_count_impl(pool);
    route_end_op(pool);
    return c;
}

size_t cmq_route_target_count(cmq_route_pool_t *pool) {
    if (!pool) return 0;
    if (route_begin_op(pool) != 0) return 0;
    cmq_mutex_lock(&pool->lock);
    size_t c = pool->target_count;
    cmq_mutex_unlock(&pool->lock);
    route_end_op(pool);
    return c;
}

int cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id,
                        cmq_route_conn_t *out) {
    if (!pool || !node_id || !out) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_get_conn_impl(pool, node_id, out);
    route_end_op(pool);
    return rc;
}

int cmq_route_snapshot(cmq_route_pool_t *pool, cmq_route_conn_t *out,
                        size_t max, size_t *out_n) {
    if (!pool || !out || !out_n || max == 0) return -1;
    if (route_begin_op(pool) != 0) return -1;
    size_t nslots;
    cmq_mutex_lock(&pool->lock);
    nslots = pool->conn_count;
    cmq_mutex_unlock(&pool->lock);
    size_t n = 0;
    for (size_t i = 0; i < nslots && n < max; i++) {
        cmq_mutex_lock(&pool->io_locks[i]);
        if (pool->conns[i].remote_id[0] || pool->conns[i].fd >= 0)
            out[n++] = pool->conns[i];
        cmq_mutex_unlock(&pool->io_locks[i]);
    }
    *out_n = n;
    route_end_op(pool);
    return 0;
}

int cmq_route_peer_live(cmq_route_pool_t *pool, const char *node_id) {
    if (!pool || !node_id) return 0;
    if (route_begin_op(pool) != 0) return 0;
    int rc = route_peer_live_impl(pool, node_id);
    route_end_op(pool);
    return rc;
}

int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass) {
    if (!pool) return -1;
    if (route_begin_op(pool) != 0) return -1;
    int rc = route_connect_impl(pool, node_id, addr, port, auth_user, auth_pass);
    route_end_op(pool);
    return rc;
}

int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd,
                        const char *auth_user, const char *auth_pass) {
    if (!pool) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (route_begin_op(pool) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    int rc = route_add_conn_impl(pool, node_id, fd, auth_user, auth_pass);
    route_end_op(pool);
    return rc;
}
