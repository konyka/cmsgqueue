#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_platform.h"
#include "cmq_coro.h"
#ifdef CMQ_OS_LINUX
#include <sys/eventfd.h>
#endif

static __thread int cmq_current_worker_id = -1;

static uint64_t srv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int wakeup_fd_create(void) {
#ifdef CMQ_OS_LINUX
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    return fds[0];
#endif
}

static void wakeup_fd_signal(int fd) {
#ifdef CMQ_OS_LINUX
    uint64_t val = 1;
    write(fd, &val, sizeof(val));
#else
    char c = 1;
    write(fd + 1, &c, 1);
#endif
}

static void wakeup_fd_drain(int fd) {
#ifdef CMQ_OS_LINUX
    uint64_t val;
    while (read(fd, &val, sizeof(val)) > 0) {}
#else
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
#endif
}

static void wakeup_fd_close(int fd) {
#ifdef CMQ_OS_LINUX
    close(fd);
#else
    close(fd);
    close(fd + 1);
#endif
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void client_read_cb(int fd, int events, void *data);
static void cmq_client_destroy(cmq_client_t *c);
static void client_teardown(cmq_client_t *c);
static int cmq_client_send_direct(cmq_client_t *c, const uint8_t *data, size_t len);
static int cmq_client_send(cmq_client_t *c, const uint8_t *data, size_t len);
static void cmq_send_message(cmq_client_t *c, uint32_t sub_id,
                              const char *subject,
                              const uint8_t *payload, size_t payload_len,
                              const uint8_t *headers, size_t headers_len);

static void worker_wakeup_cb(int fd, int events, void *data) {
    (void)events;
    cmq_worker_t *w = (cmq_worker_t *)data;
    wakeup_fd_drain(fd);

    cmq_mutex_lock(&w->msg_lock);
    cmq_worker_msg_t *msg = w->msg_head;
    w->msg_head = NULL;
    w->msg_tail = NULL;
    cmq_mutex_unlock(&w->msg_lock);

    while (msg) {
        cmq_worker_msg_t *next = msg->next;
        cmq_mutex_lock(&w->clients_lock);
        cmq_client_t *target = NULL;
        for (int i = 0; i < w->clients_count; i++) {
            if (w->clients[i] && w->clients[i]->id == msg->target_id) {
                target = w->clients[i];
                break;
            }
        }
        if (target && target->state != CMQ_CLIENT_CLOSED &&
            target->state != CMQ_CLIENT_CLOSING) {
            cmq_client_send(target, msg->buf, msg->len);
        }
        cmq_mutex_unlock(&w->clients_lock);
        free(msg->buf);
        free(msg);
        msg = next;
    }
}

static void worker_push_msg(cmq_worker_t *w, uint32_t target_id,
                             const uint8_t *buf, size_t len) {
    cmq_worker_msg_t *msg = malloc(sizeof(cmq_worker_msg_t));
    if (!msg) return;
    msg->target_id = target_id;
    msg->buf = malloc(len);
    if (!msg->buf) { free(msg); return; }
    memcpy(msg->buf, buf, len);
    msg->len = len;
    msg->next = NULL;

    cmq_mutex_lock(&w->msg_lock);
    if (w->msg_tail) {
        w->msg_tail->next = msg;
    } else {
        w->msg_head = msg;
    }
    w->msg_tail = msg;
    cmq_mutex_unlock(&w->msg_lock);

    wakeup_fd_signal(w->wakeup_fd);
}

static void worker_coro_tick(cmq_worker_t *w);

static void worker_post_tick(void *data) {
    cmq_worker_t *w = (cmq_worker_t *)data;
    worker_coro_tick(w);
}

static void *worker_thread(void *arg) {
    cmq_worker_t *w = (cmq_worker_t *)arg;
    cmq_current_worker_id = w->worker_id;
    cmq_ev_set_post_tick(w->ev_loop, worker_post_tick, w);
    cmq_ev_run(w->ev_loop, -1);
    return NULL;
}

static cmq_worker_t *cmq_worker_create(cmq_server_t *srv, int id) {
    cmq_worker_t *w = calloc(1, sizeof(cmq_worker_t));
    if (!w) return NULL;
    w->server = srv;
    w->worker_id = id;
    w->ev_loop = cmq_ev_loop_create(1024);
    if (!w->ev_loop) { free(w); return NULL; }

    w->wakeup_fd = wakeup_fd_create();
    if (w->wakeup_fd < 0) {
        cmq_ev_loop_destroy(w->ev_loop);
        free(w);
        return NULL;
    }
    cmq_ev_add(w->ev_loop, w->wakeup_fd, CMQ_EV_READ, worker_wakeup_cb, w);

    w->clients_cap = 64;
    w->clients_count = 0;
    w->clients = calloc((size_t)w->clients_cap, sizeof(cmq_client_t *));
    cmq_mutex_init(&w->clients_lock);
    cmq_mutex_init(&w->msg_lock);
    w->msg_head = NULL;
    w->msg_tail = NULL;

    w->coro_cap = CMQ_CORO_MAX_PER_WORKER;
    w->coro_count = 0;
    w->coro_pool = calloc((size_t)w->coro_cap, sizeof(cmq_coro_t *));
    return w;
}

static void cmq_worker_destroy(cmq_worker_t *w) {
    if (!w) return;
    if (w->clients) {
        for (int i = 0; i < w->clients_count; i++) {
            cmq_client_destroy(w->clients[i]);
        }
        free(w->clients);
        w->clients = NULL;
    }
    cmq_worker_msg_t *msg = w->msg_head;
    while (msg) {
        cmq_worker_msg_t *next = msg->next;
        free(msg->buf);
        free(msg);
        msg = next;
    }
    if (w->wakeup_fd >= 0) { wakeup_fd_close(w->wakeup_fd); w->wakeup_fd = -1; }
    if (w->coro_pool) {
        for (int i = 0; i < w->coro_count; i++) {
            cmq_coro_destroy(w->coro_pool[i]);
        }
        free(w->coro_pool);
        w->coro_pool = NULL;
    }
    if (w->ev_loop) { cmq_ev_loop_destroy(w->ev_loop); w->ev_loop = NULL; }
    cmq_mutex_destroy(&w->clients_lock);
    cmq_mutex_destroy(&w->msg_lock);
}

static cmq_worker_t *client_worker(cmq_server_t *srv, cmq_client_t *c) {
    if (!srv->workers) return NULL;
    for (int i = 0; i < srv->num_workers; i++) {
        cmq_worker_t *w = &srv->workers[i];
        cmq_mutex_lock(&w->clients_lock);
        for (int j = 0; j < w->clients_count; j++) {
            if (w->clients[j] == c) {
                cmq_mutex_unlock(&w->clients_lock);
                return w;
            }
        }
        cmq_mutex_unlock(&w->clients_lock);
    }
    return NULL;
}

static cmq_client_t *cmq_client_create(int fd, uint32_t id,
                                         cmq_ev_loop_t *loop,
                                         cmq_server_t *server) {
    cmq_client_t *c = calloc(1, sizeof(cmq_client_t));
    if (!c) return NULL;
    c->fd = fd;
    c->id = id;
    c->state = CMQ_CLIENT_INIT;
    c->parser = cmq_parser_create();
    c->ev_loop = loop;
    c->server = server;
    c->write_buf = NULL;
    c->write_len = 0;
    c->write_pos = 0;
    c->next_sub_id = 1;
    c->subs = NULL;
    c->username = NULL;
    c->next = NULL;
    c->last_activity_ms = srv_now_ms();
    return c;
}

static void cmq_client_destroy(cmq_client_t *c) {
    if (!c) return;
    if (c->tls) {
        cmq_tls_session_destroy(c->tls);
        c->tls = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    if (c->parser) cmq_parser_destroy(c->parser);
    free(c->write_buf);
    free(c->ws_recv_buf);
    free(c->ws_msg_buf);
    free(c->username);
    /* If teardown already ran, c->subs is NULL. Otherwise (server shutdown)
       remove remaining refs from the sublist so free_data won't double-free. */
    cmq_sub_entry_t *s = c->subs;
    while (s) {
        cmq_sub_entry_t *next = s->next;
        if (s->ref && c->server && c->server->sublist) {
            cmq_rwlock_wrlock(&c->server->sublist_lock);
            cmq_sublist_remove(c->server->sublist, s->subject, s->ref);
            cmq_rwlock_unlock(&c->server->sublist_lock);
            free(s->ref);
            s->ref = NULL;
        }
        free(s);
        s = next;
    }
    free(c);
}

/* Remove client from its owning array, clear subscriptions from the sublist,
   drop event interest, and destroy. Safe to call once; subsequent calls no-op
   because state becomes CLOSED and fd is cleared. */
static void client_teardown(cmq_client_t *c) {
    if (!c || c->state == CMQ_CLIENT_CLOSED) return;
    cmq_server_t *srv = c->server;
    int was_connected = (c->state == CMQ_CLIENT_CONNECTED);
    c->state = CMQ_CLIENT_CLOSED;

    if (c->ev_loop && c->fd >= 0) {
        cmq_ev_del(c->ev_loop, c->fd);
    }

    /* Exact-remove every subscription so publish cannot hit a dead client. */
    cmq_sub_entry_t *s = c->subs;
    c->subs = NULL;
    while (s) {
        cmq_sub_entry_t *next = s->next;
        if (s->ref) {
            cmq_rwlock_wrlock(&srv->sublist_lock);
            cmq_sublist_remove(srv->sublist, s->subject, s->ref);
            cmq_rwlock_unlock(&srv->sublist_lock);
            free(s->ref);
            s->ref = NULL;
            uint64_t cur = cmq_atomic_load_u64(&srv->stat_subscriptions,
                                                CMQ_ATOMIC_RELAXED);
            if (cur > 0) {
                cmq_atomic_fetch_sub_u64(&srv->stat_subscriptions, 1,
                                          CMQ_ATOMIC_RELAXED);
            }
        }
        free(s);
        s = next;
    }
    c->sub_count = 0;

    /* Detach from worker or acceptor client array (swap-with-last). */
    if (c->worker_id >= 0 && srv->workers) {
        cmq_worker_t *w = &srv->workers[c->worker_id];
        cmq_mutex_lock(&w->clients_lock);
        for (int i = 0; i < w->clients_count; i++) {
            if (w->clients[i] == c) {
                w->clients[i] = w->clients[w->clients_count - 1];
                w->clients_count--;
                break;
            }
        }
        cmq_mutex_unlock(&w->clients_lock);
    } else {
        cmq_mutex_lock(&srv->clients_lock);
        for (int i = 0; i < srv->clients_count; i++) {
            if (srv->clients[i] == c) {
                srv->clients[i] = srv->clients[srv->clients_count - 1];
                srv->clients_count--;
                break;
            }
        }
        cmq_mutex_unlock(&srv->clients_lock);
    }

    uint32_t active = cmq_atomic_load_u32(&srv->active_clients, CMQ_ATOMIC_RELAXED);
    if (active > 0) {
        cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
    }
    if (was_connected) {
        uint64_t conns = cmq_atomic_load_u64(&srv->stat_connections, CMQ_ATOMIC_RELAXED);
        if (conns > 0) {
            cmq_atomic_fetch_sub_u64(&srv->stat_connections, 1, CMQ_ATOMIC_RELAXED);
        }
    }

    cmq_client_destroy(c);
}

static int ensure_write_cap(cmq_client_t *c, size_t need) {
    if (need > CMQ_WRITE_BUF_LIMIT) return -1;
    if (c->write_cap >= need) return 0;
    size_t ncap = c->write_cap ? c->write_cap * 2 : 256;
    while (ncap < need) ncap *= 2;
    if (ncap > CMQ_WRITE_BUF_LIMIT) ncap = CMQ_WRITE_BUF_LIMIT;
    if (need > ncap) return -1;
    uint8_t *nb = realloc(c->write_buf, ncap);
    if (!nb) return -1;
    c->write_buf = nb;
    c->write_cap = ncap;
    return 0;
}

static int cmq_client_send_direct(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return -1;

    if (c->write_buf && c->write_pos < c->write_len) {
        size_t remaining = c->write_len - c->write_pos;
        size_t new_len = remaining + len;
        if (new_len > CMQ_WRITE_BUF_LIMIT) {
            client_teardown(c);
            return -1;
        }
        /* Compact unsent bytes to the front, then grow capacity if needed. */
        if (c->write_pos > 0) {
            memmove(c->write_buf, c->write_buf + c->write_pos, remaining);
            c->write_pos = 0;
            c->write_len = remaining;
        }
        if (ensure_write_cap(c, new_len) != 0) return -1;
        memcpy(c->write_buf + c->write_len, data, len);
        c->write_len = new_len;
        return 0;
    }

    /* Buffer empty: reuse existing capacity when possible. */
    if (ensure_write_cap(c, len) != 0) return -1;
    memcpy(c->write_buf, data, len);
    c->write_len = len;
    c->write_pos = 0;

    cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE, client_read_cb, c);
    return 0;
}

static int cmq_client_send(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return -1;
    cmq_server_t *srv = c->server;
    int cross = srv->workers && c->worker_id >= 0 && c->worker_id != cmq_current_worker_id;

    /* WebSocket clients receive CMQ frames wrapped in WS binary frames. Per
       RFC 6455 server->client frames are unmasked. The HTTP 101 upgrade
       response is written before c->is_websocket is set, so it is never
       wrapped. worker_push_msg() copies its input, so freeing wsbuf after
       dispatch is safe for both the direct and cross-worker paths. */
    if (c->is_websocket) {
        size_t hdr_len = (len <= 125) ? 2 : (len <= 65535) ? 4 : 10;
        size_t total = hdr_len + len;
        uint8_t *wsbuf = malloc(total);
        if (!wsbuf) return -1;
        cmq_ws_frame_t wf;
        wf.fin = 1;
        wf.opcode = CMQ_WS_OPCODE_BINARY;
        wf.payload = data;
        wf.payload_len = len;
        wf.mask_key = 0;
        wf.masked = 0;
        if (cmq_ws_frame_serialize(&wf, wsbuf, total) < 0) {
            free(wsbuf);
            return -1;
        }
        int rc;
        if (!cross) {
            rc = cmq_client_send_direct(c, wsbuf, total);
        } else {
            worker_push_msg(&srv->workers[c->worker_id], c->id, wsbuf, total);
            rc = 0;
        }
        free(wsbuf);
        return rc;
    }

    if (!cross) {
        return cmq_client_send_direct(c, data, len);
    }
    cmq_worker_t *w = &srv->workers[c->worker_id];
    worker_push_msg(w, c->id, data, len);
    return 0;
}

static void cmq_send_pong(cmq_client_t *c) {
    uint8_t buf[16];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PONG, 0, NULL, 0);
    if (len > 0) cmq_client_send(c, buf, len);
}

static void cmq_send_connack(cmq_client_t *c, uint8_t code) {
    uint8_t buf[16];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNACK, 0, &code, 1);
    if (len > 0) cmq_client_send(c, buf, len);
}

static void cmq_send_suback(cmq_client_t *c, uint32_t sub_id, uint8_t code) {
    uint8_t payload[5];
    payload[0] = code;
    payload[1] = (sub_id >> 24) & 0xFF;
    payload[2] = (sub_id >> 16) & 0xFF;
    payload[3] = (sub_id >> 8) & 0xFF;
    payload[4] = sub_id & 0xFF;
    uint8_t buf[16];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_SUBACK, 0, payload, 5);
    if (len > 0) cmq_client_send(c, buf, len);
}

static void cmq_send_error(cmq_client_t *c, const char *msg) {
    uint8_t buf[256];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_ERROR, 0,
                                   (const uint8_t *)msg, strlen(msg));
    if (len > 0) cmq_client_send(c, buf, len);
}

/* Build a complete MESSAGE frame. Returns malloc'd buffer; *out_len set.
   Returns NULL on OOM or invalid headers. Caller frees. */
static uint8_t *cmq_build_message_frame(uint32_t sub_id,
                                         const char *subject,
                                         const uint8_t *payload, size_t payload_len,
                                         const uint8_t *headers, size_t headers_len,
                                         size_t *out_len) {
    if (headers_len > 0 && !headers) return NULL;
    size_t subject_len = strlen(subject);
    size_t body_len = 4 + 2 + subject_len + 2 + headers_len + 4 + payload_len;
    size_t buf_size = sizeof(cmq_frame_hdr_t) + body_len;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return NULL;

    uint8_t *p = buf + sizeof(cmq_frame_hdr_t);
    p[0] = (sub_id >> 24) & 0xFF;
    p[1] = (sub_id >> 16) & 0xFF;
    p[2] = (sub_id >> 8) & 0xFF;
    p[3] = sub_id & 0xFF;
    p += 4;

    p[0] = (subject_len >> 8) & 0xFF;
    p[1] = subject_len & 0xFF;
    p += 2;
    memcpy(p, subject, subject_len);
    p += subject_len;

    p[0] = (headers_len >> 8) & 0xFF;
    p[1] = headers_len & 0xFF;
    p += 2;
    if (headers_len > 0) memcpy(p, headers, headers_len);
    p += headers_len;

    p[0] = ((uint32_t)payload_len >> 24) & 0xFF;
    p[1] = ((uint32_t)payload_len >> 16) & 0xFF;
    p[2] = ((uint32_t)payload_len >> 8) & 0xFF;
    p[3] = (uint32_t)payload_len & 0xFF;
    p += 4;
    if (payload_len > 0 && payload) memcpy(p, payload, payload_len);

    uint8_t flags = (headers_len > 0) ? CMQ_FLAG_HEADERS : 0;
    size_t len = cmq_frame_encode(buf, buf_size, CMQ_OP_MESSAGE, flags,
                                   buf + sizeof(cmq_frame_hdr_t), body_len);
    if (len == 0) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

static void cmq_patch_message_sub_id(uint8_t *buf, uint32_t sub_id) {
    uint8_t *p = buf + sizeof(cmq_frame_hdr_t);
    p[0] = (sub_id >> 24) & 0xFF;
    p[1] = (sub_id >> 16) & 0xFF;
    p[2] = (sub_id >> 8) & 0xFF;
    p[3] = sub_id & 0xFF;
}

static void cmq_send_message(cmq_client_t *c, uint32_t sub_id,
                              const char *subject,
                              const uint8_t *payload, size_t payload_len,
                              const uint8_t *headers, size_t headers_len) {
    size_t len = 0;
    uint8_t *buf = cmq_build_message_frame(sub_id, subject, payload, payload_len,
                                            headers, headers_len, &len);
    if (!buf) return;
    cmq_client_send(c, buf, len);
    free(buf);
}

static void cmq_send_request_message(cmq_client_t *c, uint32_t sub_id,
                                       const char *subject,
                                       const char *reply_to,
                                       const uint8_t *payload, size_t payload_len) {
    size_t subject_len = strlen(subject);
    size_t reply_len = reply_to ? strlen(reply_to) : 0;
    size_t body_len = 4 + 2 + subject_len + 2 + reply_len + 4 + payload_len;
    size_t buf_size = sizeof(cmq_frame_hdr_t) + body_len;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return;

    uint8_t *p = buf + sizeof(cmq_frame_hdr_t);
    p[0] = (sub_id >> 24) & 0xFF;
    p[1] = (sub_id >> 16) & 0xFF;
    p[2] = (sub_id >> 8) & 0xFF;
    p[3] = sub_id & 0xFF;
    p += 4;

    p[0] = (subject_len >> 8) & 0xFF;
    p[1] = subject_len & 0xFF;
    p += 2;
    memcpy(p, subject, subject_len);
    p += subject_len;

    p[0] = (reply_len >> 8) & 0xFF;
    p[1] = reply_len & 0xFF;
    p += 2;
    if (reply_len > 0) memcpy(p, reply_to, reply_len);
    p += reply_len;

    p[0] = ((uint32_t)payload_len >> 24) & 0xFF;
    p[1] = ((uint32_t)payload_len >> 16) & 0xFF;
    p[2] = ((uint32_t)payload_len >> 8) & 0xFF;
    p[3] = (uint32_t)payload_len & 0xFF;
    p += 4;
    memcpy(p, payload, payload_len);

    size_t len = cmq_frame_encode(buf, buf_size, CMQ_OP_MESSAGE, 0,
                                    buf + sizeof(cmq_frame_hdr_t), body_len);
    if (len > 0) {
        cmq_client_send(c, buf, len);
    }
    free(buf);
}

typedef struct {
    cmq_client_t *client;
    uint32_t sub_id;
    char queue_group[CMQ_MAX_QUEUE_GROUP];
} cmq_sub_ref_t;

/* Value snapshot for async (coroutine) delivery — no live ref/client pointers. */
typedef struct {
    uint32_t client_id;
    int worker_id;
    uint32_t sub_id;
    char queue_group[CMQ_MAX_QUEUE_GROUP];
    char account_name[CMQ_ACCOUNT_NAME_SIZE];
} cmq_deliver_tgt_t;

/* Resolve a live client by stable id. Caller must not hold clients_lock.
   Returns NULL if the client is gone or closing. */
static cmq_client_t *find_live_client(cmq_server_t *srv, uint32_t client_id,
                                       int worker_id) {
    if (worker_id >= 0 && srv->workers && worker_id < srv->num_workers) {
        cmq_worker_t *w = &srv->workers[worker_id];
        cmq_mutex_lock(&w->clients_lock);
        for (int i = 0; i < w->clients_count; i++) {
            cmq_client_t *c = w->clients[i];
            if (c && c->id == client_id &&
                (c->state == CMQ_CLIENT_CONNECTED || c->state == CMQ_CLIENT_INIT)) {
                cmq_mutex_unlock(&w->clients_lock);
                return c;
            }
        }
        cmq_mutex_unlock(&w->clients_lock);
        return NULL;
    }
    cmq_mutex_lock(&srv->clients_lock);
    for (int i = 0; i < srv->clients_count; i++) {
        cmq_client_t *c = srv->clients[i];
        if (c && c->id == client_id &&
            (c->state == CMQ_CLIENT_CONNECTED || c->state == CMQ_CLIENT_INIT)) {
            cmq_mutex_unlock(&srv->clients_lock);
            return c;
        }
    }
    cmq_mutex_unlock(&srv->clients_lock);
    return NULL;
}

/* Build a queue-group-deduped target list from match results.
   Must be called while holding sublist_lock (rd). Returns malloc'd array;
   *out_n set to count. Caller frees. */
static cmq_deliver_tgt_t *snapshot_deliver_targets(cmq_sublist_result_t *result,
                                                    size_t *out_n) {
    *out_n = 0;
    if (result->count == 0) return NULL;
    cmq_deliver_tgt_t *tgts = malloc(result->count * sizeof(cmq_deliver_tgt_t));
    if (!tgts) return NULL;

    const char *seen_qg[64];
    int nseen = 0;
    size_t n = 0;
    for (size_t i = 0; i < result->count; i++) {
        cmq_sub_ref_t *ref = (cmq_sub_ref_t *)result->entries[i];
        if (!ref || !ref->client) continue;
        if (ref->client->state != CMQ_CLIENT_CONNECTED &&
            ref->client->state != CMQ_CLIENT_INIT) continue;

        if (ref->queue_group[0] != '\0') {
            int skip = 0;
            for (int s = 0; s < nseen; s++) {
                if (strcmp(seen_qg[s], ref->queue_group) == 0) {
                    skip = 1;
                    break;
                }
            }
            if (skip) continue;
            if (nseen < 64) seen_qg[nseen++] = ref->queue_group;
        }

        tgts[n].client_id = ref->client->id;
        tgts[n].worker_id = ref->client->worker_id;
        tgts[n].sub_id = ref->sub_id;
        memcpy(tgts[n].queue_group, ref->queue_group, CMQ_MAX_QUEUE_GROUP);
        memcpy(tgts[n].account_name, ref->client->account_name,
               CMQ_ACCOUNT_NAME_SIZE);
        n++;
    }
    *out_n = n;
    return tgts;
}

/* Sync fan-out. Caller MUST hold srv->sublist_lock as a read lock so
   teardown (which takes the write lock) cannot free refs mid-delivery.
   Builds the MESSAGE frame once and patches sub_id per subscriber. */
static void deliver_matches(cmq_server_t *srv, cmq_sublist_result_t *result,
                            const char *subject,
                            const uint8_t *payload, size_t payload_len,
                            const uint8_t *headers, size_t headers_len) {
    size_t flen = 0;
    uint8_t *frame = cmq_build_message_frame(0, subject, payload, payload_len,
                                              headers, headers_len, &flen);
    if (!frame) return;

    const char *seen_qg[64];
    int nseen = 0;

    for (size_t i = 0; i < result->count; i++) {
        cmq_sub_ref_t *ref = (cmq_sub_ref_t *)result->entries[i];
        if (!ref || !ref->client) continue;
        if (ref->client->state != CMQ_CLIENT_CONNECTED &&
            ref->client->state != CMQ_CLIENT_INIT) continue;

        if (ref->queue_group[0] != '\0') {
            int skip = 0;
            for (int s = 0; s < nseen; s++) {
                if (strcmp(seen_qg[s], ref->queue_group) == 0) {
                    skip = 1;
                    break;
                }
            }
            if (skip) continue;
            if (nseen < 64) seen_qg[nseen++] = ref->queue_group;
        }

        cmq_patch_message_sub_id(frame, ref->sub_id);
        cmq_client_send(ref->client, frame, flen);
        cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1, CMQ_ATOMIC_RELAXED);
        {
            cmq_account_t *oacc = cmq_account_get(srv->accounts,
                                                   ref->client->account_name);
            if (oacc) cmq_account_inc_msgs_out(oacc, (uint64_t)payload_len);
        }
    }
    free(frame);
}

typedef struct {
    cmq_server_t *srv;
    cmq_deliver_tgt_t *targets;
    size_t target_count;
    char subject[CMQ_MAX_SUBJECT];
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *headers;
    size_t headers_len;
    uint8_t *frame;                 /* MESSAGE template; sub_id patched per target */
    size_t frame_len;
    size_t idx;
    cmq_coro_t *coro;
} cmq_deliver_ctx_t;

static void deliver_coro_func(void *arg) {
    cmq_deliver_ctx_t *ctx = (cmq_deliver_ctx_t *)arg;
    cmq_server_t *srv = ctx->srv;
    size_t batch_count = 0;

    if (!ctx->frame) {
        ctx->frame = cmq_build_message_frame(0, ctx->subject, ctx->payload,
                                              ctx->payload_len, ctx->headers,
                                              ctx->headers_len, &ctx->frame_len);
        if (!ctx->frame) goto done;
    }

    for (size_t i = ctx->idx; i < ctx->target_count; i++) {
        cmq_deliver_tgt_t *t = &ctx->targets[i];
        cmq_client_t *client = find_live_client(srv, t->client_id, t->worker_id);
        if (!client) continue;

        cmq_patch_message_sub_id(ctx->frame, t->sub_id);
        cmq_client_send(client, ctx->frame, ctx->frame_len);
        cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1, CMQ_ATOMIC_RELAXED);
        {
            cmq_account_t *oacc = cmq_account_get(srv->accounts, t->account_name);
            if (oacc) cmq_account_inc_msgs_out(oacc, (uint64_t)ctx->payload_len);
        }

        batch_count++;
        if (batch_count >= CMQ_CORO_DELIVER_BATCH) {
            ctx->idx = i + 1;
            cmq_coro_yield();
            batch_count = 0;
        }
    }

done:
    /* Ownership transferred to deliver_ctx_free via NULLed fields. */
    free((void *)ctx->payload);
    ctx->payload = NULL;
    if (ctx->headers) {
        free((void *)ctx->headers);
        ctx->headers = NULL;
    }
    free(ctx->targets);
    ctx->targets = NULL;
    free(ctx->frame);
    ctx->frame = NULL;
}

static void deliver_ctx_free(cmq_deliver_ctx_t *ctx) {
    if (!ctx) return;
    free((void *)ctx->payload);
    if (ctx->headers) free((void *)ctx->headers);
    free(ctx->targets);
    free(ctx->frame);
    free(ctx);
}

static void worker_coro_tick(cmq_worker_t *w) {
    if (!w->coro_pool || w->coro_count == 0) return;
    int write_idx = 0;
    for (int i = 0; i < w->coro_count; i++) {
        cmq_coro_t *coro = w->coro_pool[i];
        cmq_coro_state_t state = cmq_coro_state(coro);
        if (state == CMQ_CORO_DONE) {
            deliver_ctx_free((cmq_deliver_ctx_t *)coro->arg);
            cmq_coro_destroy(coro);
            continue;
        }
        if (state == CMQ_CORO_READY || state == CMQ_CORO_SUSPENDED) {
            cmq_coro_resume(coro);
        }
        state = cmq_coro_state(coro);
        if (state != CMQ_CORO_DONE) {
            w->coro_pool[write_idx++] = coro;
        } else {
            deliver_ctx_free((cmq_deliver_ctx_t *)coro->arg);
            cmq_coro_destroy(coro);
        }
    }
    w->coro_count = write_idx;
}

/* Spawn coroutine delivery from a pre-built target snapshot (no live refs).
   Takes ownership of targets/payload/headers on success; frees on failure. */
static void worker_coro_spawn_deliver(cmq_worker_t *w,
                                       cmq_server_t *srv,
                                       cmq_deliver_tgt_t *targets,
                                       size_t target_count,
                                       const char *subject,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       const uint8_t *headers,
                                       size_t headers_len) {
    cmq_deliver_ctx_t *ctx = calloc(1, sizeof(cmq_deliver_ctx_t));
    if (!ctx) {
        free(targets);
        free((void *)payload);
        if (headers) free((void *)headers);
        return;
    }
    ctx->srv = srv;
    ctx->targets = targets;
    ctx->target_count = target_count;
    strncpy(ctx->subject, subject, CMQ_MAX_SUBJECT - 1);
    ctx->payload = payload;
    ctx->payload_len = payload_len;
    ctx->headers = headers;
    ctx->headers_len = headers_len;
    ctx->idx = 0;

    cmq_coro_t *coro = cmq_coro_create(deliver_coro_func, ctx, 32768);
    if (!coro) {
        deliver_ctx_free(ctx);
        return;
    }
    ctx->coro = coro;

    if (w->coro_count < w->coro_cap) {
        w->coro_pool[w->coro_count++] = coro;
    } else {
        /* Pool full: drain synchronously so we never truncate fan-out. */
        while (cmq_coro_state(coro) != CMQ_CORO_DONE) {
            cmq_coro_resume(coro);
        }
        deliver_ctx_free(ctx);
        cmq_coro_destroy(coro);
    }
}

static void handle_publish(cmq_server_t *srv, cmq_client_t *c,
                            const cmq_frame_t *frame) {
    (void)c;
    if (!frame->payload || frame->payload_len < 2) {
        cmq_send_error(c, "invalid publish");
        return;
    }

    uint16_t subject_len = ((uint16_t)frame->payload[0] << 8) | frame->payload[1];
    if ((size_t)(2 + subject_len) > frame->payload_len) {
        cmq_send_error(c, "subject too long");
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    if (subject_len >= CMQ_MAX_SUBJECT) subject_len = CMQ_MAX_SUBJECT - 1;
    memcpy(subject, frame->payload + 2, subject_len);
    subject[subject_len] = '\0';

    size_t offset = 2 + subject_len;
    if (offset + 2 <= frame->payload_len) {
        uint16_t reply_len = ((uint16_t)frame->payload[offset] << 8) |
                              frame->payload[offset + 1];
        offset += 2 + reply_len;
    }

    const uint8_t *headers = NULL;
    size_t headers_len = 0;
    if (frame->hdr.flags & CMQ_FLAG_HEADERS) {
        if (offset + 2 <= frame->payload_len) {
            headers_len = ((uint16_t)frame->payload[offset] << 8) |
                           frame->payload[offset + 1];
            offset += 2;
            if (offset + headers_len <= frame->payload_len) {
                headers = frame->payload + offset;
            }
            offset += headers_len;
        }
    }

    const uint8_t *msg_payload = frame->payload + offset;
    size_t msg_len = frame->payload_len - offset;

    if (srv->config.max_payload_size > 0 &&
        msg_len > (size_t)srv->config.max_payload_size) {
        cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_error(c, "payload too large");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);
    cmq_atomic_fetch_add_u64(&srv->stat_bytes_in, (uint64_t)frame->payload_len,
                              CMQ_ATOMIC_RELAXED);

    cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name);
    if (acc) cmq_account_inc_msgs_in(acc, (uint64_t)frame->payload_len);

    if (srv->routes) {
        uint8_t fwd_buf[8192];
        size_t fwd_len = cmq_frame_encode(fwd_buf, sizeof(fwd_buf), CMQ_OP_PUBLISH, 0,
                                           frame->payload, frame->payload_len);
        if (fwd_len > 0) {
            cmq_route_broadcast(srv->routes, fwd_buf, fwd_len, NULL);
        }
    }

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    cmq_sublist_match(srv->sublist, subject, &result);

    if (result.count == 0) {
        cmq_sublist_result_free(&result);
        cmq_rwlock_unlock(&srv->sublist_lock);
        return;
    }

    if (result.count > CMQ_CORO_DELIVER_BATCH && srv->num_workers > 0) {
        /* Snapshot targets under the read lock, then unlock before async work. */
        size_t ntgt = 0;
        cmq_deliver_tgt_t *tgts = snapshot_deliver_targets(&result, &ntgt);
        cmq_sublist_result_free(&result);
        cmq_rwlock_unlock(&srv->sublist_lock);
        if (!tgts || ntgt == 0) {
            free(tgts);
            return;
        }

        cmq_worker_t *w = &srv->workers[cmq_current_worker_id >= 0 ?
                                         cmq_current_worker_id : 0];
        uint8_t *coro_payload = malloc(msg_len ? msg_len : 1);
        uint8_t *coro_headers = NULL;
        if (!coro_payload) {
            free(tgts);
            return;
        }
        if (msg_len > 0) memcpy(coro_payload, msg_payload, msg_len);
        if (headers_len > 0 && headers) {
            coro_headers = malloc(headers_len);
            if (!coro_headers) {
                free(coro_payload);
                free(tgts);
                return;
            }
            memcpy(coro_headers, headers, headers_len);
        }

        worker_coro_spawn_deliver(w, srv, tgts, ntgt, subject,
                                   coro_payload, msg_len,
                                   coro_headers, headers_len);
        return;
    }

    /* Sync path: hold read lock so teardown cannot free refs mid-send. */
    deliver_matches(srv, &result, subject, msg_payload, msg_len,
                    headers, headers_len);
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);
}

static void handle_subscribe(cmq_server_t *srv, cmq_client_t *c,
                              const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 6) {
        cmq_send_suback(c, 0, 1);
        return;
    }

    uint32_t sub_id = ((uint32_t)frame->payload[0] << 24) |
                      ((uint32_t)frame->payload[1] << 16) |
                      ((uint32_t)frame->payload[2] << 8) |
                      (uint32_t)frame->payload[3];
    uint16_t subject_len = ((uint16_t)frame->payload[4] << 8) |
                            frame->payload[5];
    if ((size_t)(6 + subject_len) > frame->payload_len || subject_len >= CMQ_MAX_SUBJECT) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + 6, subject_len);
    subject[subject_len] = '\0';

    char queue_group[CMQ_MAX_QUEUE_GROUP] = {0};
    size_t qg_offset = 6 + subject_len;
    if (qg_offset + 2 <= frame->payload_len) {
        uint16_t qg_len = ((uint16_t)frame->payload[qg_offset] << 8) |
                           frame->payload[qg_offset + 1];
        if (qg_len > 0 && qg_len < CMQ_MAX_QUEUE_GROUP &&
            qg_offset + 2 + qg_len <= frame->payload_len) {
            memcpy(queue_group, frame->payload + qg_offset + 2, qg_len);
            queue_group[qg_len] = '\0';
        }
    }

    int sub_cap = srv->config.max_subs_per_client > 0
                      ? srv->config.max_subs_per_client
                      : CMQ_MAX_SUBS_PER_CLIENT;
    if (c->sub_count >= sub_cap) {
        cmq_atomic_fetch_add_u64(&srv->stat_subscribes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_suback(c, sub_id, 1);
        return;
    }

    cmq_sub_entry_t *entry = malloc(sizeof(cmq_sub_entry_t));
    if (!entry) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    entry->sub_id = sub_id;
    strncpy(entry->subject, subject, CMQ_MAX_SUBJECT - 1);
    entry->subject[CMQ_MAX_SUBJECT - 1] = '\0';
    strncpy(entry->queue_group, queue_group, CMQ_MAX_QUEUE_GROUP - 1);
    entry->queue_group[CMQ_MAX_QUEUE_GROUP - 1] = '\0';
    entry->ref = NULL;
    entry->next = NULL;

    cmq_sub_ref_t *ref = malloc(sizeof(cmq_sub_ref_t));
    if (!ref) {
        free(entry);
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    ref->client = c;
    ref->sub_id = sub_id;
    strncpy(ref->queue_group, queue_group, CMQ_MAX_QUEUE_GROUP - 1);
    ref->queue_group[CMQ_MAX_QUEUE_GROUP - 1] = '\0';

    cmq_rwlock_wrlock(&srv->sublist_lock);
    int irc = cmq_sublist_insert(srv->sublist, subject, ref);
    cmq_rwlock_unlock(&srv->sublist_lock);
    if (irc != 0) {
        free(ref);
        free(entry);
        cmq_send_suback(c, sub_id, 1);
        return;
    }

    entry->ref = ref;
    entry->next = c->subs;
    c->subs = entry;

    cmq_atomic_fetch_add_u64(&srv->stat_subscriptions, 1, CMQ_ATOMIC_RELAXED);
    c->sub_count++;
    cmq_send_suback(c, sub_id, 0);
}

static void handle_unsubscribe(cmq_server_t *srv, cmq_client_t *c,
                                const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 4) return;

    uint32_t sub_id = ((uint32_t)frame->payload[0] << 24) |
                      ((uint32_t)frame->payload[1] << 16) |
                      ((uint32_t)frame->payload[2] << 8) |
                      (uint32_t)frame->payload[3];

    cmq_sub_entry_t **pp = &c->subs;
    while (*pp) {
        if ((*pp)->sub_id == sub_id) {
            cmq_sub_entry_t *entry = *pp;
            *pp = entry->next;

            cmq_rwlock_wrlock(&srv->sublist_lock);
            if (entry->ref) {
                cmq_sublist_remove(srv->sublist, entry->subject, entry->ref);
                free(entry->ref);
                entry->ref = NULL;
            }
            cmq_rwlock_unlock(&srv->sublist_lock);

            free(entry);
            if (c->sub_count > 0) c->sub_count--;
            uint64_t cur = cmq_atomic_load_u64(&srv->stat_subscriptions,
                                                CMQ_ATOMIC_RELAXED);
            if (cur > 0) {
                cmq_atomic_fetch_sub_u64(&srv->stat_subscriptions, 1,
                                          CMQ_ATOMIC_RELAXED);
            }
            break;
        }
        pp = &(*pp)->next;
    }

    uint8_t ack[16];
    size_t len = cmq_frame_encode(ack, sizeof(ack), CMQ_OP_UNSUBACK, 0,
                                   frame->payload, 4);
    if (len > 0) cmq_client_send(c, ack, len);
}

static void handle_request(cmq_server_t *srv, cmq_client_t *c,
                            const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 4) {
        cmq_send_error(c, "invalid request");
        return;
    }

    size_t offset = 0;
    uint16_t subject_len = ((uint16_t)frame->payload[offset] << 8) |
                            frame->payload[offset + 1];
    offset += 2;
    if (offset + subject_len > frame->payload_len) {
        cmq_send_error(c, "subject too long");
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    if (subject_len >= CMQ_MAX_SUBJECT) subject_len = CMQ_MAX_SUBJECT - 1;
    memcpy(subject, frame->payload + offset, subject_len);
    subject[subject_len] = '\0';
    offset += subject_len;

    uint16_t reply_len = 0;
    char reply_to[CMQ_MAX_SUBJECT] = {0};
    if (offset + 2 <= frame->payload_len) {
        reply_len = ((uint16_t)frame->payload[offset] << 8) |
                     frame->payload[offset + 1];
        offset += 2;
        if (reply_len > 0 && offset + reply_len <= frame->payload_len) {
            if (reply_len >= CMQ_MAX_SUBJECT) reply_len = CMQ_MAX_SUBJECT - 1;
            memcpy(reply_to, frame->payload + offset, reply_len);
            reply_to[reply_len] = '\0';
            offset += reply_len;
        }
    }

    const uint8_t *msg_payload = frame->payload + offset;
    size_t msg_len = frame->payload_len - offset;

    if (srv->config.max_payload_size > 0 &&
        msg_len > (size_t)srv->config.max_payload_size) {
        cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_error(c, "payload too large");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    cmq_sublist_match(srv->sublist, subject, &result);

    const char *seen_qg[64];
    int nseen = 0;
    for (size_t i = 0; i < result.count; i++) {
        cmq_sub_ref_t *ref = (cmq_sub_ref_t *)result.entries[i];
        if (!ref || !ref->client) continue;
        if (ref->client->state != CMQ_CLIENT_CONNECTED &&
            ref->client->state != CMQ_CLIENT_INIT) continue;
        if (ref->queue_group[0] != '\0') {
            int skip = 0;
            for (int s = 0; s < nseen; s++) {
                if (strcmp(seen_qg[s], ref->queue_group) == 0) {
                    skip = 1;
                    break;
                }
            }
            if (skip) continue;
            if (nseen < 64) seen_qg[nseen++] = ref->queue_group;
        }
        cmq_send_request_message(ref->client, ref->sub_id, subject,
                                  reply_to, msg_payload, msg_len);
        cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1,
                                  CMQ_ATOMIC_RELAXED);
    }
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);

    uint8_t ack[4] = {0};
    size_t ack_len = cmq_frame_encode(ack, sizeof(ack), CMQ_OP_PUBACK, 0, NULL, 0);
    if (ack_len > 0) cmq_client_send(c, ack, ack_len);
}

static void handle_response(cmq_server_t *srv, cmq_client_t *c,
                             const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 4) {
        cmq_send_error(c, "invalid response");
        return;
    }

    size_t offset = 0;
    uint16_t subject_len = ((uint16_t)frame->payload[offset] << 8) |
                            frame->payload[offset + 1];
    offset += 2;
    if (offset + subject_len > frame->payload_len) return;
    char subject[CMQ_MAX_SUBJECT];
    if (subject_len >= CMQ_MAX_SUBJECT) subject_len = CMQ_MAX_SUBJECT - 1;
    memcpy(subject, frame->payload + offset, subject_len);
    subject[subject_len] = '\0';
    offset += subject_len;

    const uint8_t *msg_payload = frame->payload + offset;
    size_t msg_len = frame->payload_len - offset;

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    cmq_sublist_match(srv->sublist, subject, &result);
    deliver_matches(srv, &result, subject, msg_payload, msg_len, NULL, 0);
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);
}

static void handle_stats(cmq_server_t *srv, cmq_client_t *c) {
    uint64_t conn = cmq_atomic_load_u64(&srv->stat_connections, CMQ_ATOMIC_RELAXED);
    uint64_t msg_in = cmq_atomic_load_u64(&srv->stat_messages_in, CMQ_ATOMIC_RELAXED);
    uint64_t msg_out = cmq_atomic_load_u64(&srv->stat_messages_out, CMQ_ATOMIC_RELAXED);
    uint64_t bytes_in = cmq_atomic_load_u64(&srv->stat_bytes_in, CMQ_ATOMIC_RELAXED);
    uint64_t bytes_out = cmq_atomic_load_u64(&srv->stat_bytes_out, CMQ_ATOMIC_RELAXED);
    uint64_t subs = cmq_atomic_load_u64(&srv->stat_subscriptions, CMQ_ATOMIC_RELAXED);
    uint64_t pub_rej = cmq_atomic_load_u64(&srv->stat_publishes_rejected,
                                             CMQ_ATOMIC_RELAXED);
    uint64_t sub_rej = cmq_atomic_load_u64(&srv->stat_subscribes_rejected,
                                             CMQ_ATOMIC_RELAXED);

    int active = 0;
    cmq_mutex_lock(&srv->clients_lock);
    active += srv->clients_count;
    cmq_mutex_unlock(&srv->clients_lock);
    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_mutex_lock(&srv->workers[i].clients_lock);
            active += srv->workers[i].clients_count;
            cmq_mutex_unlock(&srv->workers[i].clients_lock);
        }
    }

    uint8_t payload[9 * 8 + 4];
    size_t off = 0;
    #define WRITE_U64(v) do { \
        for (int b = 56; b >= 0; b -= 8) payload[off++] = (uint8_t)((v) >> b); \
    } while(0)
    WRITE_U64(conn);
    WRITE_U64(msg_in);
    WRITE_U64(msg_out);
    WRITE_U64(bytes_in);
    WRITE_U64(bytes_out);
    WRITE_U64(subs);
    payload[off++] = (active >> 24) & 0xFF;
    payload[off++] = (active >> 16) & 0xFF;
    payload[off++] = (active >> 8) & 0xFF;
    payload[off++] = active & 0xFF;
    WRITE_U64(pub_rej);
    WRITE_U64(sub_rej);
    #undef WRITE_U64

    uint8_t buf[96];
    size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_STATS, 0, payload, off);
    if (len > 0) cmq_client_send(c, buf, len);
}

static void handle_batch(cmq_server_t *srv, cmq_client_t *c,
                          const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 2) {
        cmq_send_error(c, "invalid batch");
        return;
    }
    uint16_t count = ((uint16_t)frame->payload[0] << 8) | frame->payload[1];
    size_t offset = 2;

    for (uint16_t msg = 0; msg < count && offset < frame->payload_len; msg++) {
        if (offset + 2 > frame->payload_len) break;
        uint16_t subject_len = ((uint16_t)frame->payload[offset] << 8) |
                                frame->payload[offset + 1];
        offset += 2;
        if (offset + subject_len > frame->payload_len) break;
        char subject[CMQ_MAX_SUBJECT];
        if (subject_len >= CMQ_MAX_SUBJECT) subject_len = CMQ_MAX_SUBJECT - 1;
        memcpy(subject, frame->payload + offset, subject_len);
        subject[subject_len] = '\0';
        offset += subject_len;

        if (offset + 2 > frame->payload_len) break;
        uint16_t reply_len = ((uint16_t)frame->payload[offset] << 8) |
                              frame->payload[offset + 1];
        offset += 2 + reply_len;

        if (offset + 4 > frame->payload_len) break;
        uint32_t payload_len = ((uint32_t)frame->payload[offset] << 24) |
                                ((uint32_t)frame->payload[offset + 1] << 16) |
                                ((uint32_t)frame->payload[offset + 2] << 8) |
                                (uint32_t)frame->payload[offset + 3];
        offset += 4;
        if (offset + payload_len > frame->payload_len) {
            payload_len = (uint32_t)(frame->payload_len - offset);
        }
        if (srv->config.max_payload_size > 0 &&
            payload_len > (uint32_t)srv->config.max_payload_size) {
            cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                      CMQ_ATOMIC_RELAXED);
            cmq_send_error(c, "payload too large");
            return;
        }
        const uint8_t *msg_payload = frame->payload + offset;
        offset += payload_len;

        cmq_rwlock_rdlock(&srv->sublist_lock);
        cmq_sublist_result_t result;
        cmq_sublist_match(srv->sublist, subject, &result);
        deliver_matches(srv, &result, subject, msg_payload, payload_len, NULL, 0);
        cmq_sublist_result_free(&result);
        cmq_rwlock_unlock(&srv->sublist_lock);

        cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);
    }
}

static void handle_frame(cmq_server_t *srv, cmq_client_t *c,
                          const cmq_frame_t *frame) {
    switch (frame->hdr.op) {
    case CMQ_OP_CONNECT:
        if (srv->config.auth_username) {
            if (!frame->payload || frame->payload_len < 4) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            uint16_t ulen = ((uint16_t)frame->payload[0] << 8) |
                             frame->payload[1];
            uint16_t plen = ((uint16_t)frame->payload[2] << 8) |
                             frame->payload[3];
            if ((size_t)(4 + ulen + plen) > frame->payload_len) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            char uname[256] = {0};
            char passwd[256] = {0};
            if (ulen > 0 && ulen < 256) memcpy(uname, frame->payload + 4, ulen);
            if (plen > 0 && plen < 256) memcpy(passwd, frame->payload + 4 + ulen, plen);
            if (strcmp(uname, srv->config.auth_username) != 0 ||
                strcmp(passwd, srv->config.auth_password ? srv->config.auth_password : "") != 0) {
                cmq_send_connack(c, 2);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            c->username = strdup(uname);
        }
        c->state = CMQ_CLIENT_CONNECTED;
        cmq_atomic_fetch_add_u64(&srv->stat_connections, 1, CMQ_ATOMIC_RELAXED);
        strncpy(c->account_name, "$default", CMQ_ACCOUNT_NAME_SIZE - 1);
        cmq_account_t *acc = cmq_account_get(srv->accounts, "$default");
        cmq_account_inc_connections(acc);
        cmq_send_connack(c, 0);
        break;
    case CMQ_OP_PING:
        cmq_send_pong(c);
        break;
    case CMQ_OP_PUBLISH:
        handle_publish(srv, c, frame);
        break;
    case CMQ_OP_REQUEST:
        handle_request(srv, c, frame);
        break;
    case CMQ_OP_RESPONSE:
        handle_response(srv, c, frame);
        break;
    case CMQ_OP_SUBSCRIBE:
        handle_subscribe(srv, c, frame);
        break;
    case CMQ_OP_UNSUBSCRIBE:
        handle_unsubscribe(srv, c, frame);
        break;
    case CMQ_OP_DISCONNECT:
        c->state = CMQ_CLIENT_CLOSING;
        break;
    case CMQ_OP_STATS:
        handle_stats(srv, c);
        break;
    case CMQ_OP_BATCH:
        handle_batch(srv, c, frame);
        break;
    default:
        cmq_send_error(c, "unknown op");
        break;
    }
}

static void client_flush_write(cmq_client_t *c) {
    if (!c->write_buf || c->write_pos >= c->write_len) {
        c->write_len = 0;
        c->write_pos = 0;
        /* Keep write_buf/write_cap for reuse on the next send. */
        if (c->fd >= 0)
            cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c);
        return;
    }

    size_t remaining = c->write_len - c->write_pos;
    ssize_t n = write(c->fd, c->write_buf + c->write_pos, remaining);
    if (n > 0) {
        c->write_pos += (size_t)n;
        cmq_atomic_fetch_add_u64(&c->server->stat_bytes_out, (uint64_t)n,
                                  CMQ_ATOMIC_RELAXED);
        if (c->write_pos >= c->write_len) {
            c->write_len = 0;
            c->write_pos = 0;
            cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c);
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        c->state = CMQ_CLIENT_CLOSING;
    }
}

static void send_info_frame(cmq_server_t *srv, cmq_client_t *c) {
    uint8_t info_buf[256];
    uint64_t conns = cmq_atomic_load_u64(&srv->stat_connections, CMQ_ATOMIC_RELAXED);
    uint64_t subs = cmq_atomic_load_u64(&srv->stat_subscriptions, CMQ_ATOMIC_RELAXED);
    char info_json[256];
    int info_len = snprintf(info_json, sizeof(info_json),
        "{\"version\":\"0.1.0\",\"proto\":1,\"connections\":%llu,\"subscriptions\":%llu,\"auth\":%s}",
        (unsigned long long)conns, (unsigned long long)subs,
        srv->config.auth_username ? "true" : "false");
    size_t len = cmq_frame_encode(info_buf, sizeof(info_buf), CMQ_OP_INFO, 0,
                                   (const uint8_t *)info_json, (size_t)info_len);
    if (len > 0) cmq_client_send(c, info_buf, len);
    c->info_sent = 1;
}

static int handle_ws_upgrade(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (len < 4) return -1;
    char req[4096];
    if (len > sizeof(req) - 1) len = sizeof(req) - 1;
    memcpy(req, data, len);
    req[len] = '\0';

    if (strstr(req, "Upgrade: websocket") == NULL &&
        strstr(req, "Upgrade: WebSocket") == NULL) return -1;

    char ws_key[128] = {0};
    if (cmq_ws_parse_http_upgrade(req, len, ws_key, sizeof(ws_key)) != 0) return -1;

    char accept_key[64] = {0};
    if (cmq_ws_accept_key(ws_key, accept_key, sizeof(accept_key)) != 0) return -1;

    char response[512];
    if (cmq_ws_build_response(accept_key, response, sizeof(response)) != 0) return -1;

    size_t resp_len = strlen(response);
    free(c->write_buf);
    c->write_buf = NULL;
    c->write_cap = 0;
    c->write_len = 0;
    c->write_pos = 0;
    cmq_client_send(c, (const uint8_t *)response, resp_len);

    c->is_websocket = 1;
    c->ws_upgrade_done = 1;
    return 0;
}

static void client_finish_closing(cmq_client_t *c) {
    if (!c || c->state != CMQ_CLIENT_CLOSING) return;
    if (c->write_buf && c->write_pos < c->write_len) {
        client_flush_write(c);
        if (c->state == CMQ_CLIENT_CLOSED) return; /* hard write error */
        if (c->write_buf && c->write_pos < c->write_len) {
            /* Keep socket writable so the final CONNACK/ERROR can drain. */
            if (c->fd >= 0)
                cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_WRITE, client_read_cb, c);
            return;
        }
    }
    client_teardown(c);
}

static void client_read_cb(int fd, int events, void *data) {
    cmq_client_t *c = (cmq_client_t *)data;
    cmq_server_t *srv = c->server;

    if (events & CMQ_EV_ERROR) {
        client_teardown(c);
        return;
    }

    if (events & CMQ_EV_WRITE) {
        if (c->state == CMQ_CLIENT_CLOSING) {
            client_finish_closing(c);
            return;
        }
        client_flush_write(c);
        if (c->state == CMQ_CLIENT_CLOSING || c->state == CMQ_CLIENT_CLOSED) {
            client_finish_closing(c);
            return;
        }
    }

    if (c->state == CMQ_CLIENT_CLOSING) {
        client_finish_closing(c);
        return;
    }

    if (!(events & CMQ_EV_READ)) return;

    ssize_t n = read(fd, c->read_buf, sizeof(c->read_buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            client_teardown(c);
        }
        return;
    }
    c->last_activity_ms = srv_now_ms();

    if (!c->is_websocket && !c->ws_upgrade_done && n > 0 && c->read_buf[0] == 'G') {
        if (handle_ws_upgrade(c, c->read_buf, (size_t)n) == 0) {
            return;
        }
    }

    if (!c->info_sent && !c->is_websocket) {
        send_info_frame(srv, c);
    }

    if (c->is_websocket && c->ws_upgrade_done) {
        /* Append into reassembly buffer, then drain complete WS frames. */
        size_t need = c->ws_recv_len + (size_t)n;
        if (need > c->ws_recv_cap) {
            size_t ncap = c->ws_recv_cap ? c->ws_recv_cap * 2 : 4096;
            while (ncap < need) ncap *= 2;
            /* Cap at max_payload_size + WS header overhead (~14) + slack. */
            size_t max_pl = (size_t)(srv->config.max_payload_size > 0
                                         ? srv->config.max_payload_size
                                         : CMQ_CLIENT_BUF_SIZE);
            size_t hard = max_pl + 64 + 65536; /* allow large CMQ frames */
            if (ncap > hard) ncap = hard;
            if (need > ncap) { client_teardown(c); return; }
            uint8_t *nb = realloc(c->ws_recv_buf, ncap);
            if (!nb) { client_teardown(c); return; }
            c->ws_recv_buf = nb;
            c->ws_recv_cap = ncap;
        }
        memcpy(c->ws_recv_buf + c->ws_recv_len, c->read_buf, (size_t)n);
        c->ws_recv_len += (size_t)n;

        size_t offset = 0;
        while (offset < c->ws_recv_len) {
            cmq_ws_frame_t ws_frame;
            int parsed = cmq_ws_frame_parse(c->ws_recv_buf + offset,
                                             c->ws_recv_len - offset, &ws_frame);
            if (parsed < 0) break; /* need more data */

            if (ws_frame.opcode == CMQ_WS_OPCODE_CLOSE) {
                client_teardown(c);
                return;
            }

            int is_data = (ws_frame.opcode == CMQ_WS_OPCODE_BINARY ||
                           ws_frame.opcode == CMQ_WS_OPCODE_TEXT ||
                           ws_frame.opcode == CMQ_WS_OPCODE_CONTINUATION);
            if (is_data) {
                /* Copy+unmask into message assembly buffer (never mutate
                   ws_recv_buf in place — remaining frames still reference it). */
                if (ws_frame.opcode == CMQ_WS_OPCODE_CONTINUATION && !c->ws_msg_active) {
                    client_teardown(c);
                    return;
                }
                if ((ws_frame.opcode == CMQ_WS_OPCODE_BINARY ||
                     ws_frame.opcode == CMQ_WS_OPCODE_TEXT) && c->ws_msg_active) {
                    /* New message while still assembling — protocol error. */
                    client_teardown(c);
                    return;
                }
                if (ws_frame.opcode != CMQ_WS_OPCODE_CONTINUATION)
                    c->ws_msg_active = 1;

                if (ws_frame.payload_len > 0) {
                    size_t need = c->ws_msg_len + ws_frame.payload_len;
                    if (need > c->ws_msg_cap) {
                        size_t ncap = c->ws_msg_cap ? c->ws_msg_cap * 2 : 4096;
                        while (ncap < need) ncap *= 2;
                        size_t max_pl = (size_t)(srv->config.max_payload_size > 0
                                                     ? srv->config.max_payload_size
                                                     : CMQ_CLIENT_BUF_SIZE);
                        size_t hard = max_pl + 65536;
                        if (ncap > hard) ncap = hard;
                        if (need > ncap) { client_teardown(c); return; }
                        uint8_t *nb = realloc(c->ws_msg_buf, ncap);
                        if (!nb) { client_teardown(c); return; }
                        c->ws_msg_buf = nb;
                        c->ws_msg_cap = ncap;
                    }
                    memcpy(c->ws_msg_buf + c->ws_msg_len, ws_frame.payload,
                           ws_frame.payload_len);
                    if (ws_frame.masked) {
                        cmq_ws_mask(c->ws_msg_buf + c->ws_msg_len,
                                     ws_frame.payload_len, ws_frame.mask_key);
                    }
                    c->ws_msg_len += ws_frame.payload_len;
                }

                if (ws_frame.fin) {
                    if (c->ws_msg_len > 0) {
                        int rc = cmq_parser_feed(c->parser, c->ws_msg_buf,
                                                  c->ws_msg_len);
                        if (rc < 0) { client_teardown(c); return; }
                        while (rc == 1) {
                            const cmq_frame_t *frame = cmq_parser_frame(c->parser);
                            if (frame) handle_frame(srv, c, frame);
                            if (c->state == CMQ_CLIENT_CLOSING ||
                                c->state == CMQ_CLIENT_CLOSED) {
                                client_finish_closing(c);
                                return;
                            }
                            rc = cmq_parser_next(c->parser);
                        }
                    }
                    c->ws_msg_len = 0;
                    c->ws_msg_active = 0;
                }
            }
            /* PING/PONG: consume and ignore for now. */
            offset += (size_t)parsed;
        }
        if (offset > 0) {
            size_t remain = c->ws_recv_len - offset;
            if (remain > 0) {
                memmove(c->ws_recv_buf, c->ws_recv_buf + offset, remain);
            }
            c->ws_recv_len = remain;
        }
        return;
    }

    int rc = cmq_parser_feed(c->parser, c->read_buf, (size_t)n);
    if (rc < 0) {
        client_teardown(c);
        return;
    }

    while (rc == 1) {
        const cmq_frame_t *frame = cmq_parser_frame(c->parser);
        if (frame) {
            handle_frame(srv, c, frame);
        }
        if (c->state == CMQ_CLIENT_CLOSING || c->state == CMQ_CLIENT_CLOSED) {
            client_finish_closing(c);
            return;
        }
        rc = cmq_parser_next(c->parser);
    }
}

static void keepalive_scan_clients(cmq_client_t **clients, int count,
                                    uint64_t now, uint64_t timeout_ms) {
    /* Collect first so teardown (which mutates the array) is safe. */
    cmq_client_t *doomed[256];
    int ndoomed = 0;
    for (int i = 0; i < count && ndoomed < 256; i++) {
        cmq_client_t *c = clients[i];
        if (c && c->state == CMQ_CLIENT_CONNECTED &&
            (now - c->last_activity_ms) > timeout_ms) {
            doomed[ndoomed++] = c;
        }
    }
    for (int i = 0; i < ndoomed; i++) {
        client_teardown(doomed[i]);
    }
}

static void keepalive_timer_cb(int timer_id, int events, void *data) {
    (void)timer_id;
    (void)events;
    cmq_server_t *srv = (cmq_server_t *)data;
    int interval = srv->config.ping_interval_ms;
    if (interval <= 0) return;
    uint64_t timeout_ms = (uint64_t)interval * 2;
    uint64_t now = srv_now_ms();

    cmq_mutex_lock(&srv->clients_lock);
    int n = srv->clients_count;
    cmq_client_t **snap = NULL;
    if (n > 0) {
        snap = malloc((size_t)n * sizeof(cmq_client_t *));
        if (snap) memcpy(snap, srv->clients, (size_t)n * sizeof(cmq_client_t *));
    }
    cmq_mutex_unlock(&srv->clients_lock);
    if (snap) {
        keepalive_scan_clients(snap, n, now, timeout_ms);
        free(snap);
    }

    if (srv->workers) {
        for (int wi = 0; wi < srv->num_workers; wi++) {
            cmq_worker_t *w = &srv->workers[wi];
            cmq_mutex_lock(&w->clients_lock);
            int wn = w->clients_count;
            cmq_client_t **wsnap = NULL;
            if (wn > 0) {
                wsnap = malloc((size_t)wn * sizeof(cmq_client_t *));
                if (wsnap) memcpy(wsnap, w->clients, (size_t)wn * sizeof(cmq_client_t *));
            }
            cmq_mutex_unlock(&w->clients_lock);
            if (wsnap) {
                keepalive_scan_clients(wsnap, wn, now, timeout_ms);
                free(wsnap);
            }
        }
    }
}

static int client_tls_handshake(cmq_server_t *srv, cmq_client_t *client) {
    if (!srv->tls_config) return 0;
    cmq_tls_session_t *tls = cmq_tls_server_session(srv->tls_config, client->fd);
    if (!tls) return -1;
    int rc = cmq_tls_handshake(tls);
    if (rc != 0) {
        cmq_tls_session_destroy(tls);
        return -1;
    }
    client->tls = tls;
    return 0;
}

static void accept_cb(int fd, int events, void *data) {
    cmq_server_t *srv = (cmq_server_t *)data;
    if (!(events & CMQ_EV_READ)) return;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept(fd, (struct sockaddr *)&addr, &addrlen);
    if (client_fd < 0) return;

    if (set_nonblocking(client_fd) != 0) {
        close(client_fd);
        return;
    }

    if (srv->config.max_clients > 0) {
        uint32_t cur = cmq_atomic_fetch_add_u32(&srv->active_clients, 1,
                                                 CMQ_ATOMIC_SEQ_CST);
        if ((int)(cur + 1) > srv->config.max_clients) {
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_SEQ_CST);
            close(client_fd);
            return;
        }
    } else {
        cmq_atomic_fetch_add_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
    }

    uint32_t cid = cmq_atomic_fetch_add_u32(&srv->next_client_id, 1,
                                             CMQ_ATOMIC_SEQ_CST);

    if (srv->workers && srv->num_workers > 0) {
        uint32_t wi = cmq_atomic_fetch_add_u32(&srv->next_worker, 1,
                                                CMQ_ATOMIC_RELAXED);
        int idx = (int)(wi % (uint32_t)srv->num_workers);
        cmq_worker_t *w = &srv->workers[idx];
        cmq_client_t *client = cmq_client_create(client_fd, cid,
                                                    w->ev_loop, srv);
        if (!client) {
            close(client_fd);
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
            return;
        }
        client->worker_id = idx;
        if (client_tls_handshake(srv, client) != 0) {
            cmq_client_destroy(client);
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
            return;
        }

        cmq_mutex_lock(&w->clients_lock);
        if (w->clients_count >= w->clients_cap) {
            int new_cap = w->clients_cap * 2;
            cmq_client_t **new_arr = realloc(w->clients,
                                              (size_t)new_cap * sizeof(cmq_client_t *));
            if (!new_arr) {
                cmq_mutex_unlock(&w->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                return;
            }
            w->clients = new_arr;
            w->clients_cap = new_cap;
        }
        w->clients[w->clients_count++] = client;
        cmq_mutex_unlock(&w->clients_lock);

        cmq_ev_add(w->ev_loop, client_fd, CMQ_EV_READ, client_read_cb, client);
    } else {
        cmq_client_t *client = cmq_client_create(client_fd, cid,
                                                    srv->ev_loop, srv);
        if (!client) {
            close(client_fd);
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
            return;
        }
        client->worker_id = -1;
        if (client_tls_handshake(srv, client) != 0) {
            cmq_client_destroy(client);
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
            return;
        }

        cmq_mutex_lock(&srv->clients_lock);
        if (srv->clients_count >= srv->clients_cap) {
            int new_cap = srv->clients_cap * 2;
            cmq_client_t **new_arr = realloc(srv->clients,
                                              (size_t)new_cap * sizeof(cmq_client_t *));
            if (!new_arr) {
                cmq_mutex_unlock(&srv->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                return;
            }
            srv->clients = new_arr;
            srv->clients_cap = new_cap;
        }
        srv->clients[srv->clients_count++] = client;
        cmq_mutex_unlock(&srv->clients_lock);

        cmq_ev_add(srv->ev_loop, client_fd, CMQ_EV_READ, client_read_cb, client);
    }
}

const char *cmq_version(void) {
    return CMQ_VERSION_STRING;
}

cmq_status_t cmq_server_create(cmq_server_t **server, const cmq_config_t *config) {
    if (!server) return CMQ_ERR_INVALID_ARG;

    cmq_server_t *srv = calloc(1, sizeof(cmq_server_t));
    if (!srv) return CMQ_ERR_NO_MEMORY;

    if (config) {
        srv->config = *config;
    }
    if (srv->config.port == 0) srv->config.port = CMQ_DEFAULT_PORT;
    if (!srv->config.host) srv->config.host = CMQ_DEFAULT_HOST;
    if (srv->config.max_payload_size == 0)
        srv->config.max_payload_size = CMQ_DEFAULT_MAX_PAYLOAD;
    if (srv->config.max_subs_per_client == 0)
        srv->config.max_subs_per_client = CMQ_DEFAULT_MAX_SUBS_PER_CLIENT;
    if (srv->config.ping_interval_ms == 0)
        srv->config.ping_interval_ms = CMQ_DEFAULT_PING_INTERVAL;

    srv->listen_fd = -1;
    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);

    cmq_mutex_init(&srv->clients_lock);
    cmq_rwlock_init(&srv->sublist_lock);

    srv->sublist = cmq_sublist_create();
    if (!srv->sublist) {
        free(srv);
        return CMQ_ERR_NO_MEMORY;
    }

    int log_level = srv->config.log_level;
    if (log_level == 0) log_level = 2;
    srv->log = cmq_log_create((cmq_log_level_t)log_level);
    if (srv->config.log_to_stdout) {
        cmq_log_add_stdout(srv->log);
    }
    if (srv->config.log_file && srv->config.log_to_file) {
        cmq_log_add_file(srv->log, srv->config.log_file);
    }

    srv->clients_cap = 64;
    srv->clients_count = 0;
    srv->clients = calloc((size_t)srv->clients_cap, sizeof(cmq_client_t *));

    srv->accounts = cmq_account_manager_create();
    cmq_account_create(srv->accounts, "$default");

    srv->routes = NULL;
    srv->cluster = NULL;
    srv->tls_config = NULL;

    if (srv->config.tls_enabled && srv->config.tls_cert && srv->config.tls_key) {
        srv->tls_config = cmq_tls_config_create();
        if (srv->tls_config) {
            cmq_tls_set_cert(srv->tls_config, srv->config.tls_cert);
            cmq_tls_set_key(srv->tls_config, srv->config.tls_key);
            cmq_log_info(srv->log, "TLS enabled: cert=%s", srv->config.tls_cert);
        }
    }

    if (srv->config.cluster_name && srv->config.cluster_node_id) {
        srv->cluster = cmq_cluster_create(srv->config.cluster_name,
                                           srv->config.cluster_node_id);
        if (srv->cluster) {
            srv->routes = cmq_route_pool_create(srv->cluster);
        }
    }

    *server = srv;
    return CMQ_OK;
}

cmq_status_t cmq_server_run(cmq_server_t *srv) {
    if (!srv) return CMQ_ERR_INVALID_ARG;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) return CMQ_ERR_IO;

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (set_nonblocking(srv->listen_fd) != 0) {
        close(srv->listen_fd);
        return CMQ_ERR_IO;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)srv->config.port);
    inet_pton(AF_INET, srv->config.host, &addr.sin_addr);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(srv->listen_fd);
        return CMQ_ERR_IO;
    }

    if (listen(srv->listen_fd, 512) != 0) {
        close(srv->listen_fd);
        return CMQ_ERR_IO;
    }

    srv->ev_loop = cmq_ev_loop_create(1024);
    if (!srv->ev_loop) {
        close(srv->listen_fd);
        return CMQ_ERR_NO_MEMORY;
    }

    cmq_ev_add(srv->ev_loop, srv->listen_fd, CMQ_EV_READ, accept_cb, srv);

    if (srv->config.ping_interval_ms > 0) {
        cmq_ev_timer_add(srv->ev_loop, (uint64_t)srv->config.ping_interval_ms,
                          (uint64_t)srv->config.ping_interval_ms,
                          keepalive_timer_cb, srv);
    }

    int nthreads = srv->config.num_threads;
    if (nthreads > 1) {
        srv->num_workers = nthreads;
        srv->workers = calloc((size_t)nthreads, sizeof(cmq_worker_t));
        if (!srv->workers) {
            close(srv->listen_fd);
            return CMQ_ERR_NO_MEMORY;
        }
        for (int i = 0; i < nthreads; i++) {
            cmq_worker_t *w = &srv->workers[i];
            memset(w, 0, sizeof(*w));
            w->server = srv;
            w->worker_id = i;
            w->ev_loop = cmq_ev_loop_create(1024);
            if (!w->ev_loop) {
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                return CMQ_ERR_NO_MEMORY;
            }
            w->wakeup_fd = wakeup_fd_create();
            if (w->wakeup_fd < 0) {
                cmq_ev_loop_destroy(w->ev_loop);
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                return CMQ_ERR_NO_MEMORY;
            }
            cmq_ev_add(w->ev_loop, w->wakeup_fd, CMQ_EV_READ, worker_wakeup_cb, w);
            w->clients_cap = 64;
            w->clients_count = 0;
            w->clients = calloc((size_t)w->clients_cap, sizeof(cmq_client_t *));
            cmq_mutex_init(&w->clients_lock);
            cmq_mutex_init(&w->msg_lock);
            w->msg_head = NULL;
            w->msg_tail = NULL;
            w->coro_cap = CMQ_CORO_MAX_PER_WORKER;
            w->coro_count = 0;
            w->coro_pool = calloc((size_t)w->coro_cap, sizeof(cmq_coro_t *));
        }
        for (int i = 0; i < nthreads; i++) {
            cmq_thread_create(&srv->workers[i].thread, worker_thread, &srv->workers[i]);
        }
        cmq_log_info(srv->log, "CMQ server started with %d worker threads", nthreads);
    }

    cmq_atomic_store_int(&srv->running, 1, CMQ_ATOMIC_SEQ_CST);
    cmq_log_info(srv->log, "CMQ server listening on %s:%d",
                 srv->config.host, srv->config.port);

    if (srv->routes && srv->config.route_count > 0) {
        char nid[CMQ_NODE_ID_SIZE];
        snprintf(nid, sizeof(nid), "node-%d", srv->config.port);
        for (int i = 0; i < srv->config.route_count; i++) {
            cmq_route_connect(srv->routes, nid,
                              srv->config.routes[i].addr,
                              srv->config.routes[i].port);
            cmq_log_info(srv->log, "Route connected to %s:%d",
                         srv->config.routes[i].addr,
                         srv->config.routes[i].port);
        }
    }

    cmq_ev_run(srv->ev_loop, -1);

    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_ev_stop(srv->workers[i].ev_loop);
        }
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_thread_join(srv->workers[i].thread);
        }
    }

    return CMQ_OK;
}

void cmq_server_drain(cmq_server_t *srv, int drain_timeout_ms) {
    if (!srv) return;

    if (srv->listen_fd >= 0) {
        cmq_ev_del(srv->ev_loop, srv->listen_fd);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    uint8_t disc[16];
    size_t disc_len = cmq_frame_encode(disc, sizeof(disc), CMQ_OP_DISCONNECT, 0, NULL, 0);

    cmq_mutex_lock(&srv->clients_lock);
    for (int i = 0; i < srv->clients_count; i++) {
        cmq_client_t *c = srv->clients[i];
        if (c && c->state == CMQ_CLIENT_CONNECTED) {
            if (disc_len > 0) cmq_client_send_direct(c, disc, disc_len);
            c->state = CMQ_CLIENT_CLOSING;
        }
    }
    cmq_mutex_unlock(&srv->clients_lock);

    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_worker_t *w = &srv->workers[i];
            cmq_mutex_lock(&w->clients_lock);
            for (int j = 0; j < w->clients_count; j++) {
                cmq_client_t *c = w->clients[j];
                if (c && c->state == CMQ_CLIENT_CONNECTED) {
                    if (disc_len > 0) cmq_client_send_direct(c, disc, disc_len);
                    c->state = CMQ_CLIENT_CLOSING;
                }
            }
            cmq_mutex_unlock(&w->clients_lock);
        }
    }

    struct timespec ts = {0, (long)drain_timeout_ms * 1000000L};
    nanosleep(&ts, NULL);

    cmq_server_stop(srv);
}

void cmq_server_stop(cmq_server_t *srv) {
    if (!srv) return;
    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);
    if (srv->ev_loop) cmq_ev_stop(srv->ev_loop);
    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_ev_stop(srv->workers[i].ev_loop);
        }
    }
}

void cmq_server_destroy(cmq_server_t *srv) {
    if (!srv) return;

    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_worker_destroy(&srv->workers[i]);
        }
        free(srv->workers);
    }

    if (srv->clients) {
        for (int i = 0; i < srv->clients_count; i++) {
            cmq_client_destroy(srv->clients[i]);
        }
        free(srv->clients);
    }

    if (srv->listen_fd >= 0) close(srv->listen_fd);
    if (srv->ev_loop) cmq_ev_loop_destroy(srv->ev_loop);
    if (srv->sublist) {
        cmq_sublist_free_data(srv->sublist);
        cmq_sublist_destroy(srv->sublist);
    }
    if (srv->log) cmq_log_destroy(srv->log);
    if (srv->accounts) cmq_account_manager_destroy(srv->accounts);
    if (srv->routes) cmq_route_pool_destroy(srv->routes);
    if (srv->cluster) cmq_cluster_destroy(srv->cluster);
    if (srv->tls_config) cmq_tls_config_destroy(srv->tls_config);
    cmq_mutex_destroy(&srv->clients_lock);
    cmq_rwlock_destroy(&srv->sublist_lock);
    free(srv);
}
