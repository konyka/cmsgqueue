#ifndef CMQ_SERVER_H
#define CMQ_SERVER_H

#include "cmq.h"
#include "cmq_thread.h"
#include "cmq_atomic.h"
#include "cmq_sublist.h"
#include "cmq_parser.h"
#include "cmq_ev.h"
#include "cmq_log.h"

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

#define CMQ_CLIENT_BUF_SIZE 4096
#define CMQ_MAX_SUBJECT 256
#define CMQ_MAX_SUBS_PER_CLIENT 1024

typedef enum {
    CMQ_CLIENT_INIT = 0,
    CMQ_CLIENT_CONNECTED,
    CMQ_CLIENT_CLOSING,
    CMQ_CLIENT_CLOSED
} cmq_client_state_t;

typedef struct cmq_sub_entry {
    uint32_t sub_id;
    char subject[CMQ_MAX_SUBJECT];
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

    struct cmq_client *next;
} cmq_client_t;

struct cmq_server {
    cmq_config_t config;
    int listen_fd;
    cmq_atomic_int running;

    cmq_ev_loop_t *ev_loop;
    cmq_thread_t *workers;
    int num_workers;

    cmq_client_t **clients;
    int clients_count;
    int clients_cap;
    cmq_atomic_u32 next_client_id;
    cmq_mutex_t clients_lock;

    cmq_sublist_t *sublist;
    cmq_rwlock_t sublist_lock;

    cmq_log_t *log;
};

#endif
