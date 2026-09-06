#ifndef CMQ_SERVER_H
#define CMQ_SERVER_H

#include "cmq.h"
#include "cmq_thread.h"
#include "cmq_atomic.h"
#include "cmq_sublist.h"
#include "cmq_parser.h"
#include "cmq_ev.h"
#include "cmq_log.h"
#include "cmq_account.h"
#include "cmq_route.h"
#include "cmq_ws.h"
#include "cmq_coro.h"
#include "cmq_tls.h"
#include "cmq_mpool.h"
#include "cmq_rch.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#ifdef CMQ_OS_LINUX
#include <sys/eventfd.h>
#endif

#define CMQ_CLIENT_BUF_SIZE   4096
#define CMQ_MAX_SUBJECT       256
#define CMQ_MAX_QUEUE_GROUP   64
#define CMQ_MAX_SUBS_PER_CLIENT 1024
#define CMQ_BATCH_MAX         256
#define CMQ_CORO_DELIVER_BATCH 16
#define CMQ_CORO_MAX_PER_WORKER 256
#define CMQ_WRITE_BUF_LIMIT   (4 * 1024 * 1024)
#define CMQ_WORKER_MSG_QUEUE_MAX 8192
#define CMQ_WORKER_MSG_FREELIST_MAX 64   /* P3 v0.5.7: cap freelist growth */
/* TEARDOWN may exceed SEND cap so closes are not starved; hard ceiling still. */
#define CMQ_WORKER_TEARDOWN_SLACK 4096

typedef enum {
    CMQ_CLIENT_INIT = 0,
    CMQ_CLIENT_CONNECTED,
    CMQ_CLIENT_CLOSING,
    CMQ_CLIENT_CLOSED
} cmq_client_state_t;

typedef struct cmq_sub_entry {
    uint32_t sub_id;
    char subject[CMQ_MAX_SUBJECT];
    char queue_group[CMQ_MAX_QUEUE_GROUP];
    void *ref;                      /* cmq_sub_ref_t* owned by sublist until removed */
    struct cmq_sub_entry *next;
} cmq_sub_entry_t;

/* Forward declaration */
struct cmq_server;
typedef struct cmq_server cmq_server_t;

typedef struct cmq_client {
    int fd;
    uint32_t id;
    uint32_t conn_gen;              /* distinguishes id reuse after wrap */
    cmq_client_state_t state;
    cmq_parser_t *parser;
    cmq_ev_loop_t *ev_loop;
    cmq_server_t *server;           /* back-reference to server */

    uint8_t read_buf[CMQ_CLIENT_BUF_SIZE];

    uint8_t *write_buf;
    size_t write_len;
    size_t write_pos;
    size_t write_cap;               /* allocated capacity of write_buf */

    uint32_t next_sub_id;
    cmq_sub_entry_t *subs;
    int sub_count;
    cmq_atomic_int inbox_pending;
    uint8_t trace_id[16];             /* F11: connection trace ID */
    char trace_hex[33];               /* v0.5.44: pre-encoded for logs */
    char *username;
    char account_name[CMQ_ACCOUNT_NAME_SIZE];
    uint32_t account_epoch;         /* must match account->epoch after CONNECT */
    uint64_t account_max_payload;   /* v0.5.48: cached at CONNECT; 0=unlimited */
    uint64_t account_max_bytes_live; /* v0.5.52: cached; 0=unlimited */
    int session_accounted;          /* 1 after CONNECT inc; teardown must dec */
    int is_websocket;
    int is_route;                   /* 1 = cluster route peer (no re-forward) */
    int ws_upgrade_done;
    int info_sent;
    int worker_id;
    uint64_t last_activity_ms;
    uint64_t last_write_progress_ms; /* for write_timeout_ms stall detection */
    cmq_tls_session_t *tls;
    int tls_slot;                     /* v0.5.32: which tls_config_slot to use */

    /* WebSocket receive reassembly (partial / multi-frame TCP reads). */
    uint8_t *ws_recv_buf;
    size_t ws_recv_len;
    size_t ws_recv_cap;

    /* WebSocket message-level fragmentation (FIN=0 / CONTINUATION). */
    uint8_t *ws_msg_buf;
    size_t ws_msg_len;
    size_t ws_msg_cap;
    int ws_msg_active;              /* 1 while assembling a fragmented message */
    uint16_t ws_frag_count;         /* fragments in current message; caps DoS */

    struct cmq_client *next;
} cmq_client_t;

typedef struct cmq_worker_msg {
    uint32_t target_id;             /* client id */
    uint32_t target_gen;            /* must match client->conn_gen */
    int kind;                       /* 0=send data, 1=teardown client */
    uint32_t require_sub_id;        /* 0 = no check; else skip if sub gone */
    uint8_t credit_out;             /* 1: credit msgs_out after send_local ok */
    uint8_t drain_sync;             /* 1: drain write_buf before sync complete */
    uint32_t account_epoch;
    uint32_t payload_bytes;         /* body bytes for account bytes_out */
    char account_name[CMQ_ACCOUNT_NAME_SIZE];
    char pub_account[CMQ_ACCOUNT_NAME_SIZE]; /* non-empty: recheck may_deliver */
    uint32_t pub_account_epoch;     /* publisher CONNECT epoch for may_deliver */
    int *sync_result;               /* NULL=async; else 0 pending, 1 ok, -1 fail */
    uint8_t sync_heap;              /* 1: sync_result is cmq_req_sync_t (refcount) */
    uint8_t from_pool;              /* 1: msg->buf was allocated from msg_payload_pool; do not free() */
    uint8_t *buf;
    size_t len;
    struct cmq_worker_msg *next;
} cmq_worker_msg_t;

#define CMQ_WORKER_MSG_SEND     0
#define CMQ_WORKER_MSG_TEARDOWN 1

typedef struct cmq_worker {
    cmq_ev_loop_t *ev_loop;
    cmq_client_t **clients;
    int clients_count;
    int clients_cap;
    struct cmq_idmap *idmap;        /* client_id → client* (O(1)) */
    cmq_mutex_t clients_lock;

    int wakeup_fd;
    int wakeup_wfd;                 /* write end; same as wakeup_fd on eventfd */
    cmq_thread_t thread;
    cmq_server_t *server;
    int worker_id;

    cmq_worker_msg_t *msg_head;
    cmq_worker_msg_t *msg_tail;
    cmq_mutex_t msg_lock;
    size_t msg_pending;             /* queued SEND+TEARDOWN under msg_lock */
    /* P2 v0.5.6: per-worker msg freelist (LIFO). Reuses freed
     * cmq_worker_msg_t structs across deliveries. */
    cmq_worker_msg_t *msg_freelist;
    size_t msg_freelist_count;

    cmq_coro_t **coro_pool;
    int coro_count;
    int coro_cap;
} cmq_worker_t;

struct cmq_server {
    cmq_config_t config;
    int listen_fds[4];  /* P1 v0.5.4: per-listener listen fd */
#define listen_fd listen_fds[0]   /* back-compat alias */
#define CMQ_MAX_LISTENERS 4
    /* P5: per-listener SSL_CTX slots. Slot 0 is the primary listener.
     * Subsequent slots reserved for multi-listener (port + cert) support. */
#define CMQ_MAX_LISTENERS 4
    cmq_tls_config_t *tls_config_slots[CMQ_MAX_LISTENERS];
    int tls_config_count;
    cmq_atomic_int running;

    cmq_ev_loop_t *ev_loop;
    cmq_worker_t *workers;
    int num_workers;
    cmq_atomic_u32 next_worker;

    cmq_client_t **clients;
    int clients_count;
    int clients_cap;
    struct {
        uint32_t ip;
        uint64_t window_start_ms;
        uint32_t count;
    } rate_slots[CMQ_RATE_LIMIT_SLOTS];
    struct cmq_idmap *idmap;        /* acceptor-thread clients */
    cmq_atomic_u32 next_client_id;
    cmq_atomic_u32 next_conn_gen;   /* per-connection generation (id wrap safe) */
    cmq_mutex_t clients_lock;

    cmq_sublist_t *sublist;
    /* cmq_sublist is internally self-locking (cmq_rwlock inside cmq_sublist_t);
       no extra server-side lock is required. */

    cmq_log_t *log;

    cmq_account_manager_t *accounts;
    cmq_route_pool_t *routes;
    cmq_cluster_t *cluster;
    struct cmq_filestore *filestore;   /* F5: optional WAL; NULL = disabled */
    struct cmq_sublist_persist *persist; /* F18: optional subscription WAL; NULL = disabled */
    struct cmq_mqtt_bridge *mqtt_bridge; /* F6: optional upstream MQTT client */
    /* P5: per-listener SSL_CTX slots. tls_config_count entries used. */
    /* F14: quota. NULL = no quota. */
    struct cmq_quota *quota;
    struct cmq_idempo *idempo; /* v0.5.55: D5 pid+seq window; always on */
    struct cmq_txn *txn;       /* v0.5.60: D5 coordinator; always on */
    struct cmq_otel *otel;     /* v0.5.61: D1 span ring; always on */
    void *otlp;                /* v0.5.64: cmq_otlp_url_t*; NULL = off */
    void *jwks;                /* v0.5.65: cmq_jwks_t*; NULL = off */
    /* F16: ACL. NULL handle = no ACL. Refcounted for reload safety (P1). */
    struct cmq_rch *acl_h;
    /* F15: blocklist. NULL handle = no blocklist. Refcounted for reload. */
    struct cmq_rch *blocklist_h;
    struct cmq_subject_rl *subject_rl;

    cmq_mpool_t *msg_payload_pool;  /* pool for worker SEND msg->buf (<=64KiB); NULL = malloc fallback */

    cmq_atomic_u32 active_clients;  /* accept/teardown gate for max_clients */
    cmq_atomic_int acceptor_drain;  /* 1: acceptor loop must DISCONNECT locals */
    cmq_mutex_t rate_lock;          /* F10: protects rate_slots[] */

    cmq_atomic_u64 stat_connections;
    cmq_atomic_u64 stat_messages_in;
    cmq_atomic_u64 stat_messages_replayed;  /* P4: messages restored from WAL */
    /* P4 (v0.5.3): per-gate publish-rejection counters. */
    cmq_atomic_u64 stat_publishes_rejected_size;
    cmq_atomic_u64 stat_publishes_rejected_acl;
    cmq_atomic_u64 stat_publishes_rejected_quota;
    cmq_atomic_u64 stat_publishes_rejected_ratelimit;
    cmq_atomic_u64 stat_messages_out;
    cmq_atomic_u64 stat_bytes_in;
    cmq_atomic_u64 stat_bytes_out;
    cmq_atomic_u64 stat_subscriptions;
    cmq_atomic_u64 stat_publishes_rejected;
    cmq_atomic_u64 stat_subscribes_rejected;
    cmq_atomic_u64 stat_messages_dropped;   /* worker queue full / push OOM */
    cmq_atomic_u64 stat_persist_fail;       /* F5: filestore append failures */
    cmq_atomic_u64 qg_rr_counter;           /* queue-group round-robin pick */
    cmq_atomic_u64 stat_accept_aux;         /* v0.5.42: admits from accept_thread_func */

    cmq_thread_t route_reconn_thr;
    int route_reconn_started;               /* 1 if thread joinable */
    cmq_thread_t accept_thr;                /* v0.5.42: aux accept thread */
    int accept_thr_started;                 /* 1 if joinable */
    int workers_joinable;                   /* n worker threads still to join */
    cmq_atomic_int run_active;              /* 1 while cmq_server_run owns lifecycle */
};

/* v0.5.32: per-listener TLS slot lookup. Returns the slot index
 * (0..CMQ_MAX_LISTENERS-1) whose listen fd matches, or 0 if no
 * match. Used by accept_cb to pick the right tls_config_slot for
 * each new connection. */
int srv_find_tls_slot(cmq_server_t *srv, int lfd);

/* v0.5.36: publish a message from a non-client context (e.g., the
 * MQTT bridge relay). subject: validated against cmq_sublist rules.
 * payload/payload_len: raw message bytes (NOT owned by the helper
 * — caller must free after return). account_name: synthetic owner
 * for delivery ACL. Returns 0 on success, -1 on sublist-match OOM. */
int cmq_server_publish(cmq_server_t *srv, const char *subject,
                       const uint8_t *payload, size_t payload_len,
                       const char *account_name);

/* v0.5.39: bridge-specific WAL persist. Builds a self-describing
 * frame (magic 'CMQB' + version + topic_len + topic + payload)
 * and appends it to the server's filestore. The matching recovery
 * path is a future round. Best-effort: returns 0 on success, -1
 * on error or when the server has no filestore. */
int cmq_server_persist_bridge(cmq_server_t *srv, const char *topic,
                                const uint8_t *payload, size_t payload_len);

#endif
