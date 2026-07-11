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

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Blocking CONNECT + CONNACK so the peer accepts subsequent PUBLISH frames.
   Must run before set_nonblock. Skips INFO (sent before CONNACK). */
static int route_handshake(int fd, const char *auth_user, const char *auth_pass) {
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
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNECT, 0,
                                   payload, plen);
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
            cmq_frame_hdr_t hdr;
            memcpy(&hdr, rbuf, sizeof(hdr));
            if (hdr.magic[0] != CMQ_PROTO_MAGIC_0 ||
                hdr.magic[1] != CMQ_PROTO_MAGIC_1)
                return -1;
            uint32_t plen_f = hdr.length;
            size_t need = sizeof(cmq_frame_hdr_t) + (size_t)plen_f;
            if (need > sizeof(rbuf)) return -1;
            if (rlen < need) break;

            if (hdr.op == (uint8_t)CMQ_OP_CONNACK) {
                if (plen_f < 1) return -1;
                return rbuf[sizeof(cmq_frame_hdr_t)] == 0 ? 0 : -1;
            }
            /* Skip INFO (and any other pre-CONNACK frames). */
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

/* 0 = full write, 1 = EAGAIN with zero progress (keep fd), -1 = hard/partial
   failure (caller must close — stream may already contain a truncated frame). */
static int write_full(int fd, const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && off == 0)
                return 1;
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
};

static void mark_conn_dead(cmq_route_pool_t *pool, size_t idx, int fd) {
    cmq_mutex_lock(&pool->lock);
    if (idx < pool->conn_count && pool->conns[idx].fd == fd) {
        if (pool->conns[idx].fd >= 0) close(pool->conns[idx].fd);
        pool->conns[idx].fd = -1;
        pool->conns[idx].connected = 0;
    }
    cmq_mutex_unlock(&pool->lock);
}

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
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass) {
    if (!pool || !node_id || !addr) return -1;
    cmq_mutex_lock(&pool->lock);

    for (size_t i = 0; i < pool->conn_count; i++) {
        if (strcmp(pool->conns[i].remote_id, node_id) == 0) {
            if (pool->conns[i].connected) {
                cmq_mutex_unlock(&pool->lock);
                return 0;
            }
            /* Stale slot: replace fd instead of silently succeeding. */
            if (pool->conns[i].fd >= 0) close(pool->conns[i].fd);
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
                pool->conns[i].fd = -1;
                cmq_mutex_unlock(&pool->lock);
                return -1;
            }
            if (route_handshake(fd, auth_user, auth_pass) != 0) {
                close(fd);
                pool->conns[i].fd = -1;
                cmq_mutex_unlock(&pool->lock);
                return -1;
            }
            set_nonblock(fd);
            pool->conns[i].fd = fd;
            pool->conns[i].connected = 1;
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
    if (route_handshake(fd, auth_user, auth_pass) != 0) {
        close(fd);
        cmq_mutex_unlock(&pool->lock);
        return -1;
    }
    set_nonblock(fd);

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
            if (pool->conns[i].fd >= 0 && pool->conns[i].fd != fd)
                close(pool->conns[i].fd);
            if (fd >= 0) set_nonblock(fd);
            pool->conns[i].fd = fd;
            pool->conns[i].connected = 1;
            cmq_mutex_unlock(&pool->lock);
            return 0;
        }
    }

    if (pool->conn_count >= CMQ_ROUTE_MAX_CONNS) {
        cmq_mutex_unlock(&pool->lock);
        if (fd >= 0) close(fd);
        return -1;
    }

    if (fd >= 0) set_nonblock(fd);
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
    int fds[CMQ_ROUTE_MAX_CONNS];
    size_t idxs[CMQ_ROUTE_MAX_CONNS];
    size_t n = 0;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_route_conn_t *c = &pool->conns[i];
        if (!c->connected) continue;
        if (exclude_id && strcmp(c->remote_id, exclude_id) == 0) continue;
        fds[n] = c->fd;
        idxs[n] = i;
        n++;
    }
    cmq_mutex_unlock(&pool->lock);

    int sent = 0;
    for (size_t j = 0; j < n; j++) {
        int wr = write_full(fds[j], data, len);
        if (wr == 0) {
            cmq_mutex_lock(&pool->lock);
            if (idxs[j] < pool->conn_count &&
                pool->conns[idxs[j]].fd == fds[j] &&
                pool->conns[idxs[j]].connected) {
                pool->conns[idxs[j]].bytes_sent += (uint64_t)len;
                pool->conns[idxs[j]].msgs_sent++;
            }
            cmq_mutex_unlock(&pool->lock);
            sent++;
        } else if (wr == 1) {
            continue; /* zero-progress backpressure: keep connection */
        } else {
            mark_conn_dead(pool, idxs[j], fds[j]);
        }
    }
    return sent > 0 ? 0 : -1;
}

size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id) {
    if (!pool || !data || len == 0) return 0;
    int fds[CMQ_ROUTE_MAX_CONNS];
    size_t idxs[CMQ_ROUTE_MAX_CONNS];
    size_t n = 0;
    cmq_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->conn_count; i++) {
        cmq_route_conn_t *c = &pool->conns[i];
        if (!c->connected) continue;
        if (exclude_id && strcmp(c->remote_id, exclude_id) == 0) continue;
        fds[n] = c->fd;
        idxs[n] = i;
        n++;
    }
    cmq_mutex_unlock(&pool->lock);

    size_t sent = 0;
    for (size_t j = 0; j < n; j++) {
        int wr = write_full(fds[j], data, len);
        if (wr == 0) {
            cmq_mutex_lock(&pool->lock);
            if (idxs[j] < pool->conn_count &&
                pool->conns[idxs[j]].fd == fds[j] &&
                pool->conns[idxs[j]].connected) {
                pool->conns[idxs[j]].bytes_sent += (uint64_t)len;
                pool->conns[idxs[j]].msgs_sent++;
            }
            cmq_mutex_unlock(&pool->lock);
            sent++;
        } else if (wr == 1) {
            continue;
        } else {
            mark_conn_dead(pool, idxs[j], fds[j]);
        }
    }
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
