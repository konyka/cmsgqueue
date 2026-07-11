#ifndef CMQ_ROUTE_H
#define CMQ_ROUTE_H

#include "cmq_cluster.h"
#include <stdint.h>
#include <stddef.h>

typedef struct cmq_route_pool cmq_route_pool_t;

typedef struct {
    char remote_id[CMQ_NODE_ID_SIZE];
    int fd;
    int connected;
    uint64_t msgs_sent;
    uint64_t msgs_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} cmq_route_conn_t;

/* Blocking CONNECT+CONNACK (optionally with auth). Call before set_nonblock. */
int cmq_peer_handshake(int fd, const char *auth_user, const char *auth_pass);

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
int cmq_route_disconnect(cmq_route_pool_t *pool, const char *node_id);

int cmq_route_forward(cmq_route_pool_t *pool, const char *subject,
                       const uint8_t *data, size_t len,
                       const char *exclude_id);
size_t cmq_route_broadcast(cmq_route_pool_t *pool, const uint8_t *data,
                             size_t len, const char *exclude_id,
                             size_t *out_eagain);

size_t cmq_route_pool_count(cmq_route_pool_t *pool);
cmq_route_conn_t *cmq_route_get_conn(cmq_route_pool_t *pool, const char *node_id);

#endif
