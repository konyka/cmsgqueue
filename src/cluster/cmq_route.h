#ifndef CMQ_ROUTE_H
#define CMQ_ROUTE_H

#include "cmq_cluster.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

typedef struct cmq_route_pool cmq_route_pool_t;

typedef struct {
    char remote_id[CMQ_NODE_ID_SIZE];
    int fd;
    int connected;
    int fd_owned;                   /* 1 = pool closes fd; 0 = inbound borrow */
    uint64_t msgs_sent;
    uint64_t msgs_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} cmq_route_conn_t;

/* Blocking CONNECT+CONNACK (optionally with auth). Call before set_nonblock.
   flags: CMQ_FLAG_ROUTE for cluster route peers; 0 for gateway/leaf clients. */
int cmq_peer_handshake(int fd, const char *auth_user, const char *auth_pass,
                        uint16_t flags);

/* Nonblocking connect with poll deadline (timeout_ms). Restores blocking mode. */
int cmq_connect_timeout(int fd, const struct sockaddr *sa, socklen_t slen,
                         int timeout_ms);

cmq_route_pool_t *cmq_route_pool_create(cmq_cluster_t *cluster);
void cmq_route_pool_destroy(cmq_route_pool_t *pool);

/* auth_user/auth_pass may be NULL when the peer has no auth configured.
   Sends CONNECT and waits for CONNACK before returning (blocking handshake). */
int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass);
/* fd < 0: placeholder slot (connected=1, no I/O). fd >= 0: handshake then nonblock. */
int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd,
                        const char *auth_user, const char *auth_pass);
/* Register an already-handshaken inbound route fd (pool does not own/close it).
   Returns -1 if pool full OR a live egress already exists for node_id. */
int cmq_route_attach_inbound(cmq_route_pool_t *pool, const char *node_id, int fd);
/* Drop pool reference to fd without closing (client still owns the socket). */
void cmq_route_detach_fd(cmq_route_pool_t *pool, int fd);
int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id);

int cmq_route_forward(cmq_route_pool_t *pool, const char *subject,
                       const uint8_t *data, size_t len,
                       const char *exclude_id);
size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id,
                             size_t *out_eagain);

size_t cmq_route_pool_count(cmq_route_pool_t *pool);
/* Connected peers with a live fd (excludes placeholders / dead slots). */
size_t cmq_route_live_count(cmq_route_pool_t *pool);
/* Copy connection snapshot under lock (no live pointer after unlock). */
int cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id,
                        cmq_route_conn_t *out);
/* 1 if peer has a probed-live fd (clears sticky dead slots). */
int cmq_route_peer_live(cmq_route_pool_t *pool, const char *node_id);
/* Serialize writes on a route fd (inbound borrow + client path). Returns idx or -1. */
int cmq_route_io_lock_fd(cmq_route_pool_t *pool, int fd);
void cmq_route_io_unlock_idx(cmq_route_pool_t *pool, int idx);

#endif
