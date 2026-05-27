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
#define CMQ_CORO_DELIVER_BATCH 16   /* yield every N deliveries for fairness */
#define CMQ_CORO_MAX_PER_WORKER 256 /* max concurrent coroutines per worker */

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
    struct cmq_sub_entry *next;
} cmq_sub_entry_t;

/* Forward declaration */
struct cmq_server;
typedef struct cmq_server cmq_server_t;

typedef struct cmq_client {
    int fd;
    uint32_t id;
    cmq_client_state_t state;
    cmq_parser_t *parser;
    cmq_ev_loop_t *ev_loop;
    cmq_server_t *server;           /* back-reference to server */

    uint8_t read_buf[CMQ_CLIENT_BUF_SIZE];

    uint8_t *write_buf;
    size_t write_len;
    size_t write_pos;

    uint32_t next_sub_id;
    cmq_sub_entry_t *subs;
    char *username;
    char account_name[CMQ_ACCOUNT_NAME_SIZE];
    int is_websocket;
    int ws_upgrade_done;
    int info_sent;
    int worker_id;
    uint64_t last_activity_ms;
    cmq_tls_session_t *tls;

    struct cmq_client *next;
} cmq_client_t;

typedef struct cmq_worker_msg {
    int target_fd;
    uint8_t *buf;
    size_t len;
    struct cmq_worker_msg *next;
} cmq_worker_msg_t;

typedef struct cmq_worker {
    cmq_ev_loop_t *ev_loop;
    cmq_client_t **clients;
    int clients_count;
    int clients_cap;
    cmq_mutex_t clients_lock;

    int wakeup_fd;
    cmq_thread_t thread;
    cmq_server_t *server;
    int worker_id;

    cmq_worker_msg_t *msg_head;
    cmq_worker_msg_t *msg_tail;
    cmq_mutex_t msg_lock;

    cmq_coro_t **coro_pool;
    int coro_count;
    int coro_cap;
} cmq_worker_t;

struct cmq_server {
    cmq_config_t config;
    int listen_fd;
    cmq_atomic_int running;

    cmq_ev_loop_t *ev_loop;
    cmq_worker_t *workers;
    int num_workers;
    cmq_atomic_u32 next_worker;

    cmq_client_t **clients;
    int clients_count;
    int clients_cap;
    cmq_atomic_u32 next_client_id;
    cmq_mutex_t clients_lock;

    cmq_sublist_t *sublist;
    cmq_rwlock_t sublist_lock;

    cmq_log_t *log;

    cmq_account_manager_t *accounts;
    cmq_route_pool_t *routes;
    cmq_cluster_t *cluster;
    cmq_tls_config_t *tls_config;

    cmq_atomic_u64 stat_connections;
    cmq_atomic_u64 stat_messages_in;
    cmq_atomic_u64 stat_messages_out;
    cmq_atomic_u64 stat_bytes_in;
    cmq_atomic_u64 stat_bytes_out;
    cmq_atomic_u64 stat_subscriptions;
};

#endif
