#ifndef CMQ_ROUTE_H
#define CMQ_ROUTE_H

#include "cmq_cluster.h"
#include "cmq_atomic.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

typedef struct cmq_route_pool cmq_route_pool_t;

typedef struct {
    char remote_id[CMQ_NODE_ID_SIZE];
    char remote_addr[CMQ_NODE_ADDR_SIZE];
    int remote_port;
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

/* Zero-timeout TCP liveness: POLLHUP/ERR plus MSG_PEEK for peer FIN. */
int cmq_tcp_fd_alive(int fd);

cmq_route_pool_t *cmq_route_pool_create(cmq_cluster_t *cluster);
void cmq_route_pool_destroy(cmq_route_pool_t *pool);
/* Optional: when *gate != 0, post-dial install is aborted (server drain). */
void cmq_route_pool_set_dial_gate(cmq_route_pool_t *pool, cmq_atomic_int *gate);

/* auth_user/auth_pass may be NULL when the peer has no auth configured.
   Sends CONNECT and waits for CONNACK before returning (blocking handshake). */
int cmq_route_connect(cmq_route_pool_t *pool, const char *node_id,
                       const char *addr, int port,
                       const char *auth_user, const char *auth_pass);
/* fd < 0: placeholder slot (connected=1, no I/O). fd >= 0: handshake then nonblock. */
int cmq_route_add_conn(cmq_route_pool_t *pool, const char *node_id, int fd,
                        const char *auth_user, const char *auth_pass);
/* Register an already-handshaken inbound route fd (pool does not own/close it).
   Stages with connected=0 until cmq_route_mark_connected after CONNACK drain.
   Returns -1 if pool full OR a live egress already exists for node_id. */
int cmq_route_attach_inbound(cmq_route_pool_t *pool, const char *node_id, int fd);
/* Promote a staged inbound fd to broadcast-eligible (connected=1). */
int cmq_route_mark_connected(cmq_route_pool_t *pool, int fd);
/* Demote live fd to non-broadcast (connected=0) but keep fd for flush/io_lock.
   Used when an inbound route enters CLOSING before teardown detach. */
void cmq_route_unmark_connected_fd(cmq_route_pool_t *pool, int fd);
/* Drop pool reference to fd without closing (client still owns the socket). */
void cmq_route_detach_fd(cmq_route_pool_t *pool, int fd);
/* Claim close-ownership of an egress fd for the server client (fd_owned 1→0).
   Requires matching remote_id so a recycled fd cannot bind the wrong peer.
   Fails if not currently owned (already adopted / replaced). -1 if miss. */
int cmq_route_adopt_fd(cmq_route_pool_t *pool, int fd, const char *remote_id);
int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id);
/* Close nid only if it still holds owned expect_fd (bind-fail cleanup).
   No-op if replaced/inbound; bumps cancel only when a slot was closed. */
int cmq_route_disconnect_if_owned_fd(cmq_route_pool_t *pool, const char *node_id,
                                      int expect_fd);

int cmq_route_forward(cmq_route_pool_t *pool, const char *subject,
                       const uint8_t *data, size_t len,
                       const char *exclude_id);
/* out_eagain: peers not fully written (hard fail / vanished / queue full).
   EAGAIN is queued (v0.5.85) and is not counted here. */
size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id,
                             size_t *out_eagain);

#define CMQ_ROUTE_RETRY_MAX   32
#define CMQ_ROUTE_RETRY_BYTES 2048

/* 0 queued; 1 dropped (full); -1 bad args. */
int cmq_route_retry_offer(cmq_route_pool_t *pool, const char *node_id,
                          const uint8_t *data, size_t len);
/* Writes queued frames. Returns sent count; -1 on bad args. */
int cmq_route_retry_drain(cmq_route_pool_t *pool);
size_t cmq_route_retry_pending(cmq_route_pool_t *pool);
uint64_t cmq_route_retry_dropped(cmq_route_pool_t *pool);
uint64_t cmq_route_retry_sent(cmq_route_pool_t *pool);

size_t cmq_route_pool_count(cmq_route_pool_t *pool);
/* Connected peers with a live fd (excludes placeholders / staged / dead). */
size_t cmq_route_live_count(cmq_route_pool_t *pool);
/* Slots currently holding an fd (live or staged inbound). */
size_t cmq_route_held_count(cmq_route_pool_t *pool);
/* Configured dial targets (set even when connect/handshake failed). */
size_t cmq_route_target_count(cmq_route_pool_t *pool);
/* Copy connection snapshot under lock (no live pointer after unlock). */
int cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id,
                        cmq_route_conn_t *out);
/* v0.5.47: copy up to max occupied slots (no liveness probe). */
int cmq_route_snapshot(cmq_route_pool_t *pool, cmq_route_conn_t *out,
                        size_t max, size_t *out_n);
/* 1 if peer has a probed-live fd (clears sticky dead slots). */
int cmq_route_peer_live(cmq_route_pool_t *pool, const char *node_id);
/* Serialize writes on a route fd (inbound borrow + client path). Returns idx or -1. */
int cmq_route_io_lock_fd(cmq_route_pool_t *pool, int fd);
void cmq_route_io_unlock_idx(cmq_route_pool_t *pool, int idx);

#endif
