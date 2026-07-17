#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_config.h"
#include "cmq_platform.h"
#include "cmq_coro.h"
#include <poll.h>
#ifdef CMQ_OS_LINUX
#include <sys/eventfd.h>
#endif

static __thread int cmq_current_worker_id = -1;

/* Non-empty username or password enables auth (empty strdup "" is not). */
static int auth_configured(const cmq_server_t *srv) {
    if (!srv) return 0;
    const char *u = srv->config.auth_username;
    const char *p = srv->config.auth_password;
    return (u && u[0] != '\0') || (p && p[0] != '\0');
}

/* Soft-deleted then reactivated accounts bump epoch — old sessions stay denied. */
static int client_account_live(cmq_server_t *srv, const cmq_client_t *c) {
    if (!srv || !c || !c->account_name[0]) return 0;
    uint32_t ep = 0;
    cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, &ep);
    if (!a) return 0;
    int ok = 0;
    /* Re-check active after get() unlock (TOCTOU with soft-delete). */
    if (__atomic_load_n(&a->active, __ATOMIC_ACQUIRE)) {
        uint32_t aep = __atomic_load_n(&a->epoch, __ATOMIC_ACQUIRE);
        ok = (ep == c->account_epoch && aep == c->account_epoch);
    }
    cmq_account_release(srv->accounts, a);
    return ok;
}

static uint64_t srv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Keepalive (acceptor) reads progress/activity across threads — atomics. */
static void client_mark_write_progress(cmq_client_t *c) {
    __atomic_store_n(&c->last_write_progress_ms, srv_now_ms(), __ATOMIC_RELAXED);
}
static void client_clear_write_progress(cmq_client_t *c) {
    __atomic_store_n(&c->last_write_progress_ms, (uint64_t)0, __ATOMIC_RELAXED);
}
static int client_write_stalled(const cmq_client_t *c, uint64_t now,
                                uint64_t write_timeout_ms) {
    if (!c || write_timeout_ms == 0) return 0;
    if (c->state != CMQ_CLIENT_CONNECTED && c->state != CMQ_CLIENT_CLOSING)
        return 0;
    uint64_t prog = __atomic_load_n(&c->last_write_progress_ms, __ATOMIC_RELAXED);
    return prog != 0 && (now - prog) > write_timeout_ms;
}
static void client_touch_activity(cmq_client_t *c) {
    __atomic_store_n(&c->last_activity_ms, srv_now_ms(), __ATOMIC_RELAXED);
}
static uint64_t client_activity_ms(const cmq_client_t *c) {
    return __atomic_load_n(&c->last_activity_ms, __ATOMIC_RELAXED);
}

static cmq_mutex_t *client_clients_lock(cmq_server_t *srv, const cmq_client_t *c) {
    if (!srv || !c) return NULL;
    if (srv->workers && c->worker_id >= 0 && c->worker_id < srv->num_workers)
        return &srv->workers[c->worker_id].clients_lock;
    return &srv->clients_lock;
}

/* Length-prefixed wire subject must match C-string length (no embedded NUL). */
static int wire_cstr_exact(const char *s, size_t wire_len) {
    return s && strnlen(s, wire_len) == wire_len;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int wakeup_fd_pair(int *rfd, int *wfd) {
#ifdef CMQ_OS_LINUX
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) return -1;
    *rfd = fd;
    *wfd = fd;
    return 0;
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    *rfd = fds[0];
    *wfd = fds[1];
    return 0;
#endif
}

/* Grow clients[] under caller lock. Caps at CMQ_MAX_CLIENTS_LIMIT; rejects *2 wrap. */
static int clients_array_grow(cmq_client_t ***arr, int *cap) {
    if (!arr || !cap || !*arr) return -1;
    if (*cap >= CMQ_MAX_CLIENTS_LIMIT) return -1;
    int new_cap = *cap * 2;
    if (new_cap < *cap || new_cap > CMQ_MAX_CLIENTS_LIMIT)
        new_cap = CMQ_MAX_CLIENTS_LIMIT;
    if (new_cap <= *cap) return -1;
    cmq_client_t **na = realloc(*arr, (size_t)new_cap * sizeof(*na));
    if (!na) return -1;
    *arr = na;
    *cap = new_cap;
    return 0;
}

static int wakeup_fd_signal(int wfd) {
#ifdef CMQ_OS_LINUX
    uint64_t val = 1;
    for (;;) {
        ssize_t n = write(wfd, &val, sizeof(val));
        if (n == (ssize_t)sizeof(val)) return 0;
        if (n < 0 && errno == EINTR) continue;
        /* Counter already saturated → fd is readable; worker will wake. */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
#else
    char c = 1;
    for (;;) {
        ssize_t n = write(wfd, &c, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        /* Pipe full → pending bytes already wake the reader. */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
#endif
}

static void wakeup_fd_drain(int rfd) {
#ifdef CMQ_OS_LINUX
    uint64_t val;
    while (read(rfd, &val, sizeof(val)) > 0) {}
#else
    char buf[64];
    while (read(rfd, buf, sizeof(buf)) > 0) {}
#endif
}

static void wakeup_fd_close(int rfd, int wfd) {
    if (rfd >= 0) close(rfd);
    if (wfd >= 0 && wfd != rfd) close(wfd);
}

/* Open-addressing client_id → client* map (power-of-2, linear probe).
   keys: 0 = empty, UINT32_MAX = tombstone. */
typedef struct cmq_idmap cmq_idmap_t;
struct cmq_idmap {
    uint32_t *keys;
    cmq_client_t **vals;
    size_t cap;
    size_t live;
    size_t tombs;
};

#define CMQ_IDMAP_TOMB UINT32_MAX

static uint32_t cmq_idmap_hash(uint32_t id) {
    id ^= id >> 16;
    id *= 0x7feb352du;
    id ^= id >> 15;
    id *= 0x846ca68bu;
    id ^= id >> 16;
    return id;
}

static cmq_idmap_t *cmq_idmap_create(size_t cap) {
    if (cap < 16) cap = 16;
    size_t c = 16;
    while (c < cap) {
        if (c > SIZE_MAX / 2) return NULL;
        c <<= 1;
    }
    if (c > SIZE_MAX / sizeof(uint32_t) ||
        c > SIZE_MAX / sizeof(cmq_client_t *))
        return NULL;
    cmq_idmap_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->keys = calloc(c, sizeof(uint32_t));
    m->vals = calloc(c, sizeof(cmq_client_t *));
    if (!m->keys || !m->vals) {
        free(m->keys);
        free(m->vals);
        free(m);
        return NULL;
    }
    m->cap = c;
    return m;
}

static void cmq_idmap_destroy(cmq_idmap_t *m) {
    if (!m) return;
    free(m->keys);
    free(m->vals);
    free(m);
}

static int cmq_idmap_rehash(cmq_idmap_t *m, size_t ncap) {
    uint32_t *ok = m->keys;
    cmq_client_t **ov = m->vals;
    size_t ocap = m->cap;
    m->keys = calloc(ncap, sizeof(uint32_t));
    m->vals = calloc(ncap, sizeof(cmq_client_t *));
    if (!m->keys || !m->vals) {
        free(m->keys);
        free(m->vals);
        m->keys = ok;
        m->vals = ov;
        return -1;
    }
    m->cap = ncap;
    m->live = 0;
    m->tombs = 0;
    for (size_t i = 0; i < ocap; i++) {
        uint32_t k = ok[i];
        if (k == 0 || k == CMQ_IDMAP_TOMB) continue;
        size_t j = cmq_idmap_hash(k) & (ncap - 1);
        while (m->keys[j] != 0)
            j = (j + 1) & (ncap - 1);
        m->keys[j] = k;
        m->vals[j] = ov[i];
        m->live++;
    }
    free(ok);
    free(ov);
    return 0;
}

static int cmq_idmap_put(cmq_idmap_t *m, uint32_t id, cmq_client_t *c) {
    if (!m || id == 0 || id == CMQ_IDMAP_TOMB || !c) return -1;
    if ((m->live + m->tombs + 1) * 2 >= m->cap) {
        if (m->cap > SIZE_MAX / 2) return -1;
        size_t ncap = m->cap * 2;
        if (ncap > SIZE_MAX / sizeof(uint32_t) ||
            ncap > SIZE_MAX / sizeof(cmq_client_t *))
            return -1;
        if (cmq_idmap_rehash(m, ncap) != 0) return -1;
    }
    size_t mask = m->cap - 1;
    size_t i = cmq_idmap_hash(id) & mask;
    size_t tomb = SIZE_MAX;
    for (;;) {
        uint32_t k = m->keys[i];
        if (k == id) {
            /* Never silently remap an in-use id to another client. */
            if (m->vals[i] == c) return 0;
            return -1;
        }
        if (k == CMQ_IDMAP_TOMB) {
            if (tomb == SIZE_MAX) tomb = i;
        } else if (k == 0) {
            size_t slot = (tomb != SIZE_MAX) ? tomb : i;
            if (m->keys[slot] == CMQ_IDMAP_TOMB) m->tombs--;
            m->keys[slot] = id;
            m->vals[slot] = c;
            m->live++;
            return 0;
        }
        i = (i + 1) & mask;
    }
}

static cmq_client_t *cmq_idmap_get(const cmq_idmap_t *m, uint32_t id) {
    if (!m || !m->cap || id == 0 || id == CMQ_IDMAP_TOMB) return NULL;
    size_t mask = m->cap - 1;
    size_t i = cmq_idmap_hash(id) & mask;
    size_t start = i;
    do {
        uint32_t k = m->keys[i];
        if (k == 0) return NULL;
        if (k == id) return m->vals[i];
        i = (i + 1) & mask;
    } while (i != start);
    return NULL;
}

static void cmq_idmap_del(cmq_idmap_t *m, uint32_t id) {
    if (!m || !m->cap || id == 0 || id == CMQ_IDMAP_TOMB) return;
    size_t mask = m->cap - 1;
    size_t i = cmq_idmap_hash(id) & mask;
    size_t start = i;
    do {
        uint32_t k = m->keys[i];
        if (k == 0) return;
        if (k == id) {
            m->keys[i] = CMQ_IDMAP_TOMB;
            m->vals[i] = NULL;
            m->live--;
            m->tombs++;
            if (m->tombs > m->cap / 4 && m->live * 4 < m->cap && m->cap > 16)
                (void)cmq_idmap_rehash(m, m->cap / 2);
            return;
        }
        i = (i + 1) & mask;
    } while (i != start);
}

static void client_read_cb(int fd, int events, void *data);
static void cmq_client_destroy(cmq_client_t *c);
static void client_teardown(cmq_client_t *c);
static void client_finish_closing(cmq_client_t *c);
static void acceptor_post_tick(void *data);
static void worker_purge_send_for_id(cmq_worker_t *w, uint32_t target_id,
                                      uint32_t target_gen);
static int client_has_sub(const cmq_client_t *c, uint32_t sub_id);
static void send_info_frame(cmq_server_t *srv, cmq_client_t *c);
static int cmq_client_send_direct(cmq_client_t *c, const uint8_t *data, size_t len);
static int cmq_client_send_local(cmq_client_t *c, const uint8_t *data, size_t len);
static int client_drain_write_sync(cmq_client_t *c);
static int cmq_client_send(cmq_client_t *c, const uint8_t *data, size_t len);
static int cmq_client_send_checked(cmq_client_t *c, const uint8_t *data, size_t len,
                                    uint32_t require_sub_id);
static ssize_t client_sock_read(cmq_client_t *c, uint8_t *buf, size_t len);
static ssize_t client_sock_write(cmq_client_t *c, const uint8_t *buf, size_t len);
static void cmq_send_message(cmq_client_t *c, uint32_t sub_id,
                              const char *subject,
                              const uint8_t *payload, size_t payload_len,
                              const uint8_t *headers, size_t headers_len);
static void deliver_ctx_free(void *arg);

static int client_has_sub(const cmq_client_t *c, uint32_t sub_id) {
    if (!c || sub_id == 0) return 0;
    for (const cmq_sub_entry_t *e = c->subs; e; e = e->next) {
        if (e->sub_id == sub_id) return 1;
    }
    return 0;
}

/* Constant-time equality over n bytes (timing-safe auth compares). */
static int ct_memeq(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}

/* Match peer IP to configured routes[]. Returns route index or -1.
   Source TCP port is ephemeral on inbound peers — do not compare to routes[].port. */
static int peer_route_index(cmq_server_t *srv, int fd) {
    if (!srv || fd < 0 || srv->config.route_count <= 0) return -1;
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) return -1;
    for (int i = 0; i < srv->config.route_count; i++) {
        if (!srv->config.routes[i].addr) continue;
        struct in_addr expect;
        if (inet_pton(AF_INET, srv->config.routes[i].addr, &expect) != 1)
            continue;
        if (expect.s_addr == peer.sin_addr.s_addr)
            return i;
    }
    return -1;
}

static ssize_t client_sock_read(cmq_client_t *c, uint8_t *buf, size_t len) {
    for (;;) {
        ssize_t n = c->tls ? cmq_tls_read(c->tls, buf, len)
                           : read(c->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static ssize_t client_sock_write(cmq_client_t *c, const uint8_t *buf, size_t len) {
    for (;;) {
        ssize_t n = c->tls ? cmq_tls_write(c->tls, buf, len)
                           : write(c->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

#define CMQ_WORKER_WAKE_BATCH 64

/* Heap sync slot for cross-worker REQUEST — publisher+worker each hold a ref.
   result: 0 pending, 2 claimed (worker owns wire), 1 ok, -1 fail/abandon. */
typedef struct {
    int result;
    int refs;
} cmq_req_sync_t;

static void req_sync_release(cmq_req_sync_t *s) {
    if (!s) return;
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0)
        free(s);
}

static int req_sync_terminal(int v) {
    return v == 1 || v == -1;
}

/* Claim pending (0→2) before send so hard-abandon cannot race mid-delivery. */
static int req_sync_claim(int *result) {
    int expected = 0;
    return __atomic_compare_exchange_n(result, &expected, 2, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void worker_complete_sync(cmq_worker_msg_t *msg, int v) {
    if (!msg || !msg->sync_result) return;
    if (msg->sync_heap) {
        /* Resolve from pending/claimed only — never clobber abandon (-1). */
        for (;;) {
            int cur = __atomic_load_n(msg->sync_result, __ATOMIC_ACQUIRE);
            if (req_sync_terminal(cur)) {
                req_sync_release((cmq_req_sync_t *)msg->sync_result);
                return;
            }
            int expected = cur; /* 0 or 2 */
            if (__atomic_compare_exchange_n(msg->sync_result, &expected, v, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                req_sync_release((cmq_req_sync_t *)msg->sync_result);
                return;
            }
        }
    }
    __atomic_store_n(msg->sync_result, v, __ATOMIC_RELEASE);
}

/* MESSAGE/REQUEST body: [4B sub_id][2B slen][subject]… */
static int message_frame_subject(const uint8_t *buf, size_t len,
                                  char *out, size_t out_sz) {
    if (!buf || !out || out_sz < 2 || len < sizeof(cmq_frame_hdr_t) + 6)
        return -1;
    const uint8_t *p = buf + sizeof(cmq_frame_hdr_t);
    size_t body = len - sizeof(cmq_frame_hdr_t);
    uint16_t slen = ((uint16_t)p[4] << 8) | (uint16_t)p[5];
    if (slen == 0 || (size_t)slen >= out_sz || 6u + (size_t)slen > body)
        return -1;
    memcpy(out, p + 6, slen);
    out[slen] = '\0';
    return 0;
}

/* Deliver one SEND mailbox message (caller frees msg after). */
static void worker_deliver_send_msg(cmq_worker_t *w, cmq_worker_msg_t *msg) {
    cmq_mutex_lock(&w->clients_lock);
    cmq_client_t *target = cmq_idmap_get(w->idmap, msg->target_id);
    /* Owning worker thread: validate under lock, send after unlock so
       keepalive/teardown scans are not blocked on slow consumers. */
    int finish_dead = 0;
    int do_send = 0;
    if (target && target->conn_gen == msg->target_gen &&
        target->state != CMQ_CLIENT_CLOSED &&
        target->state != CMQ_CLIENT_CLOSING) {
        if (!client_account_live(w->server, target)) {
            target->state = CMQ_CLIENT_CLOSING;
            finish_dead = 1;
            if (w->server)
                cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
        } else if (msg->require_sub_id == 0 ||
                   client_has_sub(target, msg->require_sub_id)) {
            do_send = 1;
        } else if (w->server) {
            cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
        }
    } else if (w->server) {
        cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                  CMQ_ATOMIC_RELAXED);
    }
    cmq_mutex_unlock(&w->clients_lock);
    int sync_ok = 0;
    if (do_send && target) {
        if (!client_account_live(w->server, target)) {
            cmq_mutex_lock(&w->clients_lock);
            if (target->conn_gen == msg->target_gen &&
                target->state != CMQ_CLIENT_CLOSED &&
                target->state != CMQ_CLIENT_CLOSING)
                target->state = CMQ_CLIENT_CLOSING;
            cmq_mutex_unlock(&w->clients_lock);
            finish_dead = 1;
            if (w->server)
                cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
        } else {
            int acl_deny = 0;
            if (msg->pub_account[0] != '\0' && w->server &&
                w->server->accounts) {
                char subj[CMQ_MAX_SUBJECT];
                if (message_frame_subject(msg->buf, msg->len, subj,
                                           sizeof(subj)) != 0 ||
                    !cmq_account_may_deliver(w->server->accounts,
                                              msg->pub_account,
                                              target->account_name, subj))
                    acl_deny = 1;
            }
            if (acl_deny) {
                if (w->server)
                    cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                              CMQ_ATOMIC_RELAXED);
            } else if (msg->sync_heap && msg->sync_result &&
                       !req_sync_claim(msg->sync_result)) {
                /* Hard-abandoned or already resolved — do not deliver late. */
                if (w->server)
                    cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                              CMQ_ATOMIC_RELAXED);
            } else if (cmq_client_send_local(target, msg->buf, msg->len) != 0) {
                if (w->server)
                    cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                              CMQ_ATOMIC_RELAXED);
            } else if (msg->drain_sync &&
                       client_drain_write_sync(target) != 0) {
                if (w->server)
                    cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                              CMQ_ATOMIC_RELAXED);
            } else {
                sync_ok = 1;
                if (msg->credit_out && w->server) {
                    cmq_atomic_fetch_add_u64(&w->server->stat_messages_out, 1,
                                              CMQ_ATOMIC_RELAXED);
                    if (msg->account_name[0] != '\0') {
                        cmq_account_t *oacc =
                            cmq_account_get(w->server->accounts,
                                             msg->account_name, NULL);
                        if (oacc) {
                            cmq_account_inc_msgs_out(
                                oacc, msg->account_epoch,
                                (uint64_t)msg->payload_bytes);
                            cmq_account_release(w->server->accounts, oacc);
                        }
                    }
                }
            }
        }
    }
    worker_complete_sync(msg, sync_ok ? 1 : -1);
    if (finish_dead && target)
        client_finish_closing(target);
    free(msg->buf);
    free(msg);
}

/* Pop next SEND (skip TEARDOWN left in queue). Used to break AB REQUEST waits. */
static cmq_worker_msg_t *worker_pop_send(cmq_worker_t *w) {
    cmq_mutex_lock(&w->msg_lock);
    cmq_worker_msg_t **pp = &w->msg_head;
    cmq_worker_msg_t *prev = NULL;
    while (*pp) {
        cmq_worker_msg_t *m = *pp;
        if (m->kind == CMQ_WORKER_MSG_TEARDOWN) {
            prev = m;
            pp = &m->next;
            continue;
        }
        *pp = m->next;
        if (w->msg_tail == m)
            w->msg_tail = prev;
        if (w->msg_pending > 0)
            w->msg_pending--;
        m->next = NULL;
        if (!w->msg_head)
            w->msg_tail = NULL;
        cmq_mutex_unlock(&w->msg_lock);
        return m;
    }
    cmq_mutex_unlock(&w->msg_lock);
    return NULL;
}

static void worker_drain_sends(cmq_worker_t *w, int max_n) {
    if (!w || max_n <= 0) return;
    for (int i = 0; i < max_n; i++) {
        cmq_worker_msg_t *m = worker_pop_send(w);
        if (!m) break;
        worker_deliver_send_msg(w, m);
    }
}

static void worker_wakeup_cb(int fd, int events, void *data) {
    (void)events;
    cmq_worker_t *w = (cmq_worker_t *)data;
    wakeup_fd_drain(fd);

    for (;;) {
        cmq_mutex_lock(&w->msg_lock);
        cmq_worker_msg_t *msg = w->msg_head;
        cmq_worker_msg_t *batch = msg;
        cmq_worker_msg_t *last = NULL;
        int n = 0;
        while (msg && n < CMQ_WORKER_WAKE_BATCH) {
            last = msg;
            msg = msg->next;
            n++;
        }
        if (last)
            last->next = NULL;
        w->msg_head = msg;
        if (!msg)
            w->msg_tail = NULL;
        if (w->msg_pending >= (size_t)n)
            w->msg_pending -= (size_t)n;
        else
            w->msg_pending = 0;
        int more = (msg != NULL);
        cmq_mutex_unlock(&w->msg_lock);

        msg = batch;
        while (msg) {
            cmq_worker_msg_t *next = msg->next;
            if (msg->kind == CMQ_WORKER_MSG_TEARDOWN) {
                /* Drop queued SEND dead letters before reclaiming the client. */
                worker_purge_send_for_id(w, msg->target_id, msg->target_gen);
                cmq_mutex_lock(&w->clients_lock);
                cmq_client_t *target = cmq_idmap_get(w->idmap, msg->target_id);
                cmq_mutex_unlock(&w->clients_lock);
                if (target && target->conn_gen == msg->target_gen) {
                    if (target->state != CMQ_CLIENT_CLOSED &&
                        target->state != CMQ_CLIENT_CLOSING)
                        target->state = CMQ_CLIENT_CLOSING;
                    client_finish_closing(target);
                }
                free(msg);
                msg = next;
                continue;
            }
            worker_deliver_send_msg(w, msg);
            msg = next;
        }
        if (!more) break;
        /* Prefer re-arm; if wakeup write fails keep draining here. */
        if (w->wakeup_wfd >= 0 && wakeup_fd_signal(w->wakeup_wfd) == 0)
            break;
    }
}

/* Drop queued SEND frames for a gone client so the queue does not stay full
   of dead letters (TEARDOWN still bypasses the SEND cap). */
static void worker_purge_send_for_id(cmq_worker_t *w, uint32_t target_id,
                                      uint32_t target_gen) {
    if (!w || target_id == 0) return;
    cmq_mutex_lock(&w->msg_lock);
    cmq_worker_msg_t **pp = &w->msg_head;
    cmq_worker_msg_t *tail = NULL;
    while (*pp) {
        cmq_worker_msg_t *m = *pp;
        if (m->kind != CMQ_WORKER_MSG_TEARDOWN && m->target_id == target_id &&
            m->target_gen == target_gen) {
            *pp = m->next;
            if (w->msg_pending > 0)
                w->msg_pending--;
            worker_complete_sync(m, -1);
            if (w->server)
                cmq_atomic_fetch_add_u64(&w->server->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
            free(m->buf);
            free(m);
            continue;
        }
        tail = m;
        pp = &m->next;
    }
    w->msg_tail = tail;
    cmq_mutex_unlock(&w->msg_lock);
}

/* Returns 0 on success, -1 on OOM or queue-full.
   Callers that care about drops should count once (do not count here — retries
   would over-count). credit_out: mailbox credits msgs_out after send_local.
   sync_heap: sync_result is cmq_req_sync_t* (refcount freer). */
static int worker_push_msg(cmq_worker_t *w, uint32_t target_id,
                             uint32_t target_gen,
                             const uint8_t *buf, size_t len,
                             uint32_t require_sub_id,
                             int credit_out, const char *account_name,
                             uint32_t account_epoch, uint32_t payload_bytes,
                             int *sync_result, int drain_sync,
                             const char *pub_account, int sync_heap) {
    cmq_mutex_lock(&w->msg_lock);
    if (w->msg_pending >= CMQ_WORKER_MSG_QUEUE_MAX) {
        cmq_mutex_unlock(&w->msg_lock);
        return -1;
    }
    cmq_mutex_unlock(&w->msg_lock);

    cmq_worker_msg_t *msg = malloc(sizeof(cmq_worker_msg_t));
    if (!msg)
        return -1;
    msg->target_id = target_id;
    msg->target_gen = target_gen;
    msg->kind = CMQ_WORKER_MSG_SEND;
    msg->require_sub_id = require_sub_id;
    msg->credit_out = credit_out ? 1 : 0;
    msg->drain_sync = drain_sync ? 1 : 0;
    msg->sync_result = sync_result;
    msg->sync_heap = (sync_result && sync_heap) ? 1 : 0;
    if (sync_result && !sync_heap)
        __atomic_store_n(sync_result, 0, __ATOMIC_RELAXED);
    msg->account_epoch = account_epoch;
    msg->payload_bytes = payload_bytes;
    if (credit_out && account_name) {
        strncpy(msg->account_name, account_name, CMQ_ACCOUNT_NAME_SIZE - 1);
        msg->account_name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
    } else {
        msg->account_name[0] = '\0';
    }
    if (pub_account && pub_account[0]) {
        strncpy(msg->pub_account, pub_account, CMQ_ACCOUNT_NAME_SIZE - 1);
        msg->pub_account[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
    } else {
        msg->pub_account[0] = '\0';
    }
    msg->buf = malloc(len);
    if (!msg->buf) {
        free(msg);
        return -1;
    }
    memcpy(msg->buf, buf, len);
    msg->len = len;
    msg->next = NULL;

    cmq_mutex_lock(&w->msg_lock);
    if (w->msg_pending >= CMQ_WORKER_MSG_QUEUE_MAX) {
        cmq_mutex_unlock(&w->msg_lock);
        free(msg->buf);
        free(msg);
        return -1;
    }
    if (w->msg_tail) {
        w->msg_tail->next = msg;
    } else {
        w->msg_head = msg;
    }
    w->msg_tail = msg;
    w->msg_pending++;
    cmq_mutex_unlock(&w->msg_lock);

    if (wakeup_fd_signal(w->wakeup_wfd) == 0)
        return 0;

    /* Wakeup hard-failed — unlink if still queued so callers can retry.
       If already drained by the worker, treat as success (delivery in flight). */
    cmq_mutex_lock(&w->msg_lock);
    int found = 0;
    if (w->msg_head == msg) {
        w->msg_head = msg->next;
        if (w->msg_tail == msg) w->msg_tail = NULL;
        found = 1;
    } else {
        cmq_worker_msg_t *p = w->msg_head;
        while (p && p->next != msg) p = p->next;
        if (p) {
            p->next = msg->next;
            if (w->msg_tail == msg) w->msg_tail = p;
            found = 1;
        }
    }
    if (found && w->msg_pending > 0) w->msg_pending--;
    cmq_mutex_unlock(&w->msg_lock);
    if (!found)
        return 0;
    worker_complete_sync(msg, -1);
    free(msg->buf);
    free(msg);
    return -1;
}

/* Schedule client teardown on the owning worker thread (never cross-thread free).
   Dedupes pending TEARDOWN for the same id+gen. May exceed the SEND-only queue
   cap (keepalive/close must not stall behind fan-out), but still hard-capped by
   CMQ_WORKER_TEARDOWN_SLACK so mass-timeout cannot unbounded-malloc.
   Returns 0 on success, -1 on OOM / hard-full (callers fall back to SHUT_RDWR). */
static int worker_push_teardown(cmq_worker_t *w, uint32_t target_id,
                                 uint32_t target_gen) {
    cmq_mutex_lock(&w->msg_lock);
    for (cmq_worker_msg_t *m = w->msg_head; m; m = m->next) {
        if (m->kind == CMQ_WORKER_MSG_TEARDOWN && m->target_id == target_id &&
            m->target_gen == target_gen) {
            cmq_mutex_unlock(&w->msg_lock);
            return 0;
        }
    }
    if (w->msg_pending >= CMQ_WORKER_MSG_QUEUE_MAX + CMQ_WORKER_TEARDOWN_SLACK) {
        cmq_mutex_unlock(&w->msg_lock);
        return -1;
    }
    cmq_mutex_unlock(&w->msg_lock);

    cmq_worker_msg_t *msg = calloc(1, sizeof(cmq_worker_msg_t));
    if (!msg) return -1;
    msg->target_id = target_id;
    msg->target_gen = target_gen;
    msg->kind = CMQ_WORKER_MSG_TEARDOWN;
    /* sync_result/credit_out/drain_sync remain 0 — destroy must not write junk. */

    cmq_mutex_lock(&w->msg_lock);
    for (cmq_worker_msg_t *m = w->msg_head; m; m = m->next) {
        if (m->kind == CMQ_WORKER_MSG_TEARDOWN && m->target_id == target_id &&
            m->target_gen == target_gen) {
            cmq_mutex_unlock(&w->msg_lock);
            free(msg);
            return 0;
        }
    }
    if (w->msg_pending >= CMQ_WORKER_MSG_QUEUE_MAX + CMQ_WORKER_TEARDOWN_SLACK) {
        cmq_mutex_unlock(&w->msg_lock);
        free(msg);
        return -1;
    }
    if (w->msg_tail) {
        w->msg_tail->next = msg;
    } else {
        w->msg_head = msg;
    }
    w->msg_tail = msg;
    w->msg_pending++;
    cmq_mutex_unlock(&w->msg_lock);

    if (wakeup_fd_signal(w->wakeup_wfd) == 0)
        return 0;

    cmq_mutex_lock(&w->msg_lock);
    int found = 0;
    if (w->msg_head == msg) {
        w->msg_head = msg->next;
        if (w->msg_tail == msg) w->msg_tail = NULL;
        found = 1;
    } else {
        cmq_worker_msg_t *p = w->msg_head;
        while (p && p->next != msg) p = p->next;
        if (p) {
            p->next = msg->next;
            if (w->msg_tail == msg) w->msg_tail = p;
            found = 1;
        }
    }
    if (found && w->msg_pending > 0) w->msg_pending--;
    cmq_mutex_unlock(&w->msg_lock);
    if (!found)
        return 0;
    free(msg);
    return -1;
}

/* Teardown preferred; on OOM/queue-full nudge EOF so CLOSING cannot linger. */
static void worker_teardown_or_shutdown(cmq_worker_t *w, uint32_t target_id,
                                         uint32_t target_gen) {
    if (worker_push_teardown(w, target_id, target_gen) == 0)
        return;
    cmq_mutex_lock(&w->clients_lock);
    for (int j = 0; j < w->clients_count; j++) {
        cmq_client_t *c = w->clients[j];
        if (c && c->id == target_id && c->conn_gen == target_gen &&
            c->fd >= 0) {
            shutdown(c->fd, SHUT_RDWR);
            break;
        }
    }
    cmq_mutex_unlock(&w->clients_lock);
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

    w->wakeup_fd = -1;
    w->wakeup_wfd = -1;
    if (wakeup_fd_pair(&w->wakeup_fd, &w->wakeup_wfd) != 0) {
        cmq_ev_loop_destroy(w->ev_loop);
        free(w);
        return NULL;
    }
    if (cmq_ev_add(w->ev_loop, w->wakeup_fd, CMQ_EV_READ, worker_wakeup_cb, w) != 0) {
        wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
        cmq_ev_loop_destroy(w->ev_loop);
        free(w);
        return NULL;
    }

    w->clients_cap = 64;
    w->clients_count = 0;
    w->clients = calloc((size_t)w->clients_cap, sizeof(cmq_client_t *));
    w->idmap = cmq_idmap_create(64);
    if (!w->clients || !w->idmap) {
        free(w->clients);
        cmq_idmap_destroy(w->idmap);
        wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
        cmq_ev_loop_destroy(w->ev_loop);
        free(w);
        return NULL;
    }
    cmq_mutex_init(&w->clients_lock);
    cmq_mutex_init(&w->msg_lock);
    w->msg_head = NULL;
    w->msg_tail = NULL;

    w->coro_cap = CMQ_CORO_MAX_PER_WORKER;
    w->coro_count = 0;
    w->coro_pool = calloc((size_t)w->coro_cap, sizeof(cmq_coro_t *));
    if (!w->coro_pool) {
        cmq_mutex_destroy(&w->msg_lock);
        cmq_mutex_destroy(&w->clients_lock);
        free(w->clients);
        cmq_idmap_destroy(w->idmap);
        wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
        cmq_ev_loop_destroy(w->ev_loop);
        free(w);
        return NULL;
    }
    return w;
}

static void cmq_worker_destroy(cmq_worker_t *w) {
    if (!w) return;
    if (w->clients) {
        /* Teardown (not bare destroy) so inbound routes detach under io_lock. */
        while (w->clients_count > 0)
            client_teardown(w->clients[0]);
        free(w->clients);
        w->clients = NULL;
    }
    cmq_idmap_destroy(w->idmap);
    w->idmap = NULL;
    cmq_worker_msg_t *msg = w->msg_head;
    while (msg) {
        cmq_worker_msg_t *next = msg->next;
        worker_complete_sync(msg, -1);
        free(msg->buf);
        free(msg);
        msg = next;
    }
    if (w->wakeup_fd >= 0 || w->wakeup_wfd >= 0) {
        wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
        w->wakeup_fd = -1;
        w->wakeup_wfd = -1;
    }
    if (w->coro_pool) {
        for (int i = 0; i < w->coro_count; i++) {
            if (w->coro_pool[i]) {
                deliver_ctx_free(w->coro_pool[i]->arg);
                cmq_coro_destroy(w->coro_pool[i]);
            }
        }
        free(w->coro_pool);
        w->coro_pool = NULL;
        w->coro_count = 0;
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

static size_t cmq_client_frame_hard_cap(const cmq_server_t *srv) {
    size_t max_pl = (srv && srv->config.max_payload_size > 0)
                        ? (size_t)srv->config.max_payload_size
                        : (size_t)CMQ_CLIENT_BUF_SIZE;
    /* Subject + reply + headers (uint16) + framing — not a free 256KiB pad. */
    size_t overhead = (size_t)CMQ_MAX_SUBJECT * 2u + 65536u + 64u;
    if (max_pl > SIZE_MAX - overhead) return SIZE_MAX;
    return max_pl + overhead;
}

static cmq_client_t *cmq_client_create(int fd, uint32_t id,
                                         cmq_ev_loop_t *loop,
                                         cmq_server_t *server) {
    cmq_client_t *c = calloc(1, sizeof(cmq_client_t));
    if (!c) return NULL;
    c->fd = fd;
    c->id = id;
    if (server) {
        uint32_t g = cmq_atomic_fetch_add_u32(&server->next_conn_gen, 1,
                                               CMQ_ATOMIC_RELAXED);
        if (g == 0)
            g = cmq_atomic_fetch_add_u32(&server->next_conn_gen, 1,
                                          CMQ_ATOMIC_RELAXED);
        c->conn_gen = g;
    } else {
        c->conn_gen = 1;
    }
    c->state = CMQ_CLIENT_INIT;
    c->parser = cmq_parser_create();
    if (!c->parser) { free(c); return NULL; }
    if (server && server->config.max_payload_size > 0) {
        cmq_parser_set_max_payload(c->parser, cmq_client_frame_hard_cap(server));
    }
    c->ev_loop = loop;
    c->server = server;
    c->write_buf = NULL;
    c->write_len = 0;
    c->write_pos = 0;
    c->next_sub_id = 1;
    c->subs = NULL;
    c->username = NULL;
    c->next = NULL;
    client_touch_activity(c);
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

/* Detach route fd under pool->lock → io_lock (never hold io_lock first). */
static void route_detach_under_io_lock(cmq_server_t *srv, int fd) {
    if (!srv || !srv->routes || fd < 0) return;
    cmq_route_detach_fd(srv->routes, fd);
}

/* Remove client from its owning array, clear subscriptions from the sublist,
   drop event interest, and destroy. Safe to call once; subsequent calls no-op
   because state becomes CLOSED and fd is cleared. */
static void client_teardown(cmq_client_t *c) {
    if (!c || c->state == CMQ_CLIENT_CLOSED) return;
    cmq_server_t *srv = c->server;
    int accounted = c->session_accounted;
    c->session_accounted = 0;
    c->state = CMQ_CLIENT_CLOSED;

    if (c->is_route && srv && c->fd >= 0)
        route_detach_under_io_lock(srv, c->fd);

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
            {
                cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, NULL);
                if (a) {
                    cmq_account_dec_subscriptions(a, c->account_epoch);
                    cmq_account_release(srv->accounts, a);
                }
            }
        }
        free(s);
        s = next;
    }
    c->sub_count = 0;

    /* Detach from worker or acceptor client array (swap-with-last). */
    int wid = c->worker_id;
    uint32_t cid = c->id;
    uint32_t cgen = c->conn_gen;
    if (c->worker_id >= 0 && srv->workers) {
        cmq_worker_t *w = &srv->workers[c->worker_id];
        cmq_mutex_lock(&w->clients_lock);
        cmq_idmap_del(w->idmap, c->id);
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
        cmq_idmap_del(srv->idmap, c->id);
        for (int i = 0; i < srv->clients_count; i++) {
            if (srv->clients[i] == c) {
                srv->clients[i] = srv->clients[srv->clients_count - 1];
                srv->clients_count--;
                break;
            }
        }
        cmq_mutex_unlock(&srv->clients_lock);
    }
    /* After clients_lock: purge SEND dead letters (avoid AB-BA with wakeup). */
    if (wid >= 0 && srv->workers && wid < srv->num_workers)
        worker_purge_send_for_id(&srv->workers[wid], cid, cgen);

    uint32_t active = cmq_atomic_load_u32(&srv->active_clients, CMQ_ATOMIC_RELAXED);
    if (active > 0) {
        cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
    }
    if (accounted) {
        uint64_t conns = cmq_atomic_load_u64(&srv->stat_connections, CMQ_ATOMIC_RELAXED);
        if (conns > 0) {
            cmq_atomic_fetch_sub_u64(&srv->stat_connections, 1, CMQ_ATOMIC_RELAXED);
        }
        if (c->account_name[0] != '\0') {
            cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, NULL);
            if (acc) {
                cmq_account_dec_connections(acc, c->account_epoch);
                cmq_account_release(srv->accounts, acc);
            }
        }
    }

    cmq_client_destroy(c);
}

static int ensure_write_cap(cmq_client_t *c, size_t need) {
    if (need > CMQ_WRITE_BUF_LIMIT) return -1;
    if (c->write_cap >= need) return 0;
    size_t ncap = c->write_cap ? c->write_cap : 256;
    if (c->write_cap) {
        if (c->write_cap > SIZE_MAX / 2) ncap = CMQ_WRITE_BUF_LIMIT;
        else ncap = c->write_cap * 2;
    }
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = CMQ_WRITE_BUF_LIMIT; break; }
        ncap *= 2;
    }
    if (ncap > CMQ_WRITE_BUF_LIMIT) ncap = CMQ_WRITE_BUF_LIMIT;
    if (need > ncap) return -1;
    uint8_t *nb = realloc(c->write_buf, ncap);
    if (!nb) return -1;
    c->write_buf = nb;
    c->write_cap = ncap;
    return 0;
}

/* Mark CLOSING without teardown — callers may hold clients_lock. */
static void client_force_closing(cmq_client_t *c) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return;
    c->state = CMQ_CLIENT_CLOSING;
    if (c->ev_loop && c->fd >= 0) {
        if (cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE,
                       client_read_cb, c) != 0) {
            c->write_len = 0;
            c->write_pos = 0;
            client_clear_write_progress(c);
            (void)shutdown(c->fd, SHUT_RDWR);
        }
    }
}

static int cmq_client_send_direct(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return -1;

    int route_io_idx = -1;
    if (c->is_route && c->server && c->server->routes && c->fd >= 0) {
        route_io_idx = cmq_route_io_lock_fd(c->server->routes, c->fd);
        /* Fail-closed: never write a route fd unlocked (broadcast may hold it). */
        if (route_io_idx < 0) {
            c->state = CMQ_CLIENT_CLOSING;
            if (c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            return -1;
        }
    }

    #define CMQ_SEND_FORCE_CLOSE() client_force_closing(c)

    int rc = -1;
    if (c->write_buf && c->write_pos < c->write_len) {
        size_t remaining = c->write_len - c->write_pos;
        if (len > SIZE_MAX - remaining || remaining + len > CMQ_WRITE_BUF_LIMIT) {
            CMQ_SEND_FORCE_CLOSE();
            goto out;
        }
        size_t new_len = remaining + len;
        /* Compact unsent bytes to the front, then grow capacity if needed. */
        if (c->write_pos > 0) {
            memmove(c->write_buf, c->write_buf + c->write_pos, remaining);
            c->write_pos = 0;
            c->write_len = remaining;
        }
        if (ensure_write_cap(c, new_len) != 0) {
            if (c->server)
                cmq_atomic_fetch_add_u64(&c->server->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
            CMQ_SEND_FORCE_CLOSE();
            goto out;
        }
        memcpy(c->write_buf + c->write_len, data, len);
        c->write_len = new_len;
        if (__atomic_load_n(&c->last_write_progress_ms, __ATOMIC_RELAXED) == 0)
            client_mark_write_progress(c);
        /* Re-arm WRITE in case interest was dropped after a prior flush. */
        if (cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE,
                       client_read_cb, c) != 0) {
            size_t rem = c->write_len - c->write_pos;
            ssize_t n = client_sock_write(c, c->write_buf + c->write_pos, rem);
            if (n > 0) {
                c->write_pos += (size_t)n;
                if (c->server)
                    cmq_atomic_fetch_add_u64(&c->server->stat_bytes_out, (uint64_t)n,
                                              CMQ_ATOMIC_RELAXED);
                if (c->write_pos >= c->write_len) {
                    c->write_len = 0;
                    c->write_pos = 0;
                    client_clear_write_progress(c);
                    rc = 0;
                    goto out;
                }
            }
            c->write_len = 0;
            c->write_pos = 0;
            client_clear_write_progress(c);
            c->state = CMQ_CLIENT_CLOSING;
            if (c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            rc = -1;
            goto out;
        }
        rc = 0;
        goto out;
    }

    /* Buffer empty: reuse existing capacity when possible. */
    if (ensure_write_cap(c, len) != 0) {
        if (c->server)
            cmq_atomic_fetch_add_u64(&c->server->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
        CMQ_SEND_FORCE_CLOSE();
        goto out;
    }
    memcpy(c->write_buf, data, len);
    c->write_len = len;
    c->write_pos = 0;
    client_mark_write_progress(c);

    if (cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE,
                   client_read_cb, c) != 0) {
        /* WRITE arm failed — try one-shot drain; else force close so we do not
           leave a forever-stuck write_buf with only READ interest. */
        size_t rem = c->write_len - c->write_pos;
        ssize_t n = client_sock_write(c, c->write_buf + c->write_pos, rem);
        if (n > 0) {
            c->write_pos += (size_t)n;
            if (c->server)
                cmq_atomic_fetch_add_u64(&c->server->stat_bytes_out, (uint64_t)n,
                                          CMQ_ATOMIC_RELAXED);
            if (c->write_pos >= c->write_len) {
                c->write_len = 0;
                c->write_pos = 0;
                client_clear_write_progress(c);
                rc = 0;
                goto out;
            }
        }
        c->write_len = 0;
        c->write_pos = 0;
        client_clear_write_progress(c);
        c->state = CMQ_CLIENT_CLOSING;
        if (c->fd >= 0)
            (void)shutdown(c->fd, SHUT_RDWR);
        rc = -1;
        goto out;
    }
    rc = 0;
out:
#undef CMQ_SEND_FORCE_CLOSE
    if (route_io_idx >= 0 && c->server && c->server->routes)
        cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
    return rc;
}

/* Owning-thread send of a raw CMQ frame. Wraps WebSocket binary if needed.
   Cross-thread callers must use cmq_client_send() / worker_push_msg() instead. */
static int cmq_client_send_local(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return -1;
    int rc;
    if (!c->is_websocket) {
        rc = cmq_client_send_direct(c, data, len);
    } else {
        size_t hdr_len = (len <= 125) ? 2 : (len <= 65535) ? 4 : 10;
        if (len > SIZE_MAX - hdr_len) {
            client_force_closing(c);
            return -1;
        }
        size_t total = hdr_len + len;
        uint8_t *wsbuf = malloc(total);
        if (!wsbuf) {
            if (c->server)
                cmq_atomic_fetch_add_u64(&c->server->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
            /* Align with write_buf OOM: connection-level failure, not silent drop. */
            client_force_closing(c);
            return -1;
        }
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
        rc = cmq_client_send_direct(c, wsbuf, total);
        free(wsbuf);
    }
    /* Outbound keepalive refresh only for CONNECTED (INIT must stay frozen). */
    if (rc == 0 && c->state == CMQ_CLIENT_CONNECTED)
        client_touch_activity(c);
    return rc;
}

/* Drain write_buf before exposing fd to the route pool (handshake order).
   Cap total wait so a slow peer cannot stall the owning event thread (~256s).
   Route fds take route_io_lock — same as send_direct / flush (vs broadcast). */
#define CMQ_DRAIN_SYNC_MS 2000
static int client_drain_write_sync(cmq_client_t *c) {
    if (!c || c->fd < 0) return -1;
    int route_io_idx = -1;
    if (c->is_route && c->server && c->server->routes) {
        route_io_idx = cmq_route_io_lock_fd(c->server->routes, c->fd);
        if (route_io_idx < 0) return -1;
    }
    int rc = -1;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 128; i++) {
        if (!c->write_buf || c->write_pos >= c->write_len) {
            c->write_len = 0;
            c->write_pos = 0;
            client_clear_write_progress(c);
            rc = 0;
            goto out;
        }
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int64_t elapsed_ms =
            (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 +
            (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed_ms >= CMQ_DRAIN_SYNC_MS)
            goto out;
        size_t rem = c->write_len - c->write_pos;
        ssize_t n = client_sock_write(c, c->write_buf + c->write_pos, rem);
        if (n > 0) {
            c->write_pos += (size_t)n;
            client_mark_write_progress(c);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int wait_ms = (int)(CMQ_DRAIN_SYNC_MS - elapsed_ms);
            if (wait_ms > 200) wait_ms = 200;
            if (wait_ms < 1) goto out;
            struct pollfd pfd = { .fd = c->fd, .events = POLLOUT };
            int pr;
            do {
                pr = poll(&pfd, 1, wait_ms);
            } while (pr < 0 && errno == EINTR);
            if (pr <= 0)
                goto out;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        goto out;
    }
    if (!(c->write_buf && c->write_pos < c->write_len))
        rc = 0;
out:
    if (route_io_idx >= 0 && c->server && c->server->routes)
        cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
    return rc;
}

static int cmq_client_send_checked(cmq_client_t *c, const uint8_t *data, size_t len,
                                    uint32_t require_sub_id) {
    if (!c || c->state == CMQ_CLIENT_CLOSED || c->state == CMQ_CLIENT_CLOSING)
        return -1;
    cmq_server_t *srv = c->server;
    int cross = srv->workers && c->worker_id >= 0 && c->worker_id != cmq_current_worker_id;

    if (cross) {
        return worker_push_msg(&srv->workers[c->worker_id], c->id, c->conn_gen,
                                data, len, require_sub_id, 0, NULL, 0, 0,
                                NULL, 0, NULL, 0);
    }
    if (require_sub_id != 0 && !client_has_sub(c, require_sub_id))
        return -1;
    return cmq_client_send_local(c, data, len);
}

static int cmq_client_send(cmq_client_t *c, const uint8_t *data, size_t len) {
    return cmq_client_send_checked(c, data, len, 0);
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
    if (!subject || (headers_len > 0 && !headers)) return NULL;
    size_t subject_len = strlen(subject);
    if (subject_len == 0 || subject_len >= CMQ_MAX_SUBJECT ||
        subject_len > 0xFFFFu || headers_len > 0xFFFFu)
        return NULL;
    /* Saturating size check — reject before wraparound malloc. */
    if (payload_len > SIZE_MAX - (4u + 2u + subject_len + 2u + headers_len + 4u))
        return NULL;
    size_t body_len = 4 + 2 + subject_len + 2 + headers_len + 4 + payload_len;
    if (body_len > SIZE_MAX - sizeof(cmq_frame_hdr_t)) return NULL;
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

static uint8_t *cmq_build_request_message_frame(uint32_t sub_id,
                                                 const char *subject,
                                                 const char *reply_to,
                                                 const uint8_t *payload,
                                                 size_t payload_len,
                                                 size_t *out_len) {
    if (!subject || !out_len) return NULL;
    *out_len = 0;
    size_t subject_len = strlen(subject);
    size_t reply_len = reply_to ? strlen(reply_to) : 0;
    if (subject_len == 0 || subject_len >= CMQ_MAX_SUBJECT ||
        subject_len > 0xFFFFu || reply_len > 0xFFFFu)
        return NULL;
    if (payload_len > SIZE_MAX - (4u + 2u + subject_len + 2u + reply_len + 4u))
        return NULL;
    size_t body_len = 4 + 2 + subject_len + 2 + reply_len + 4 + payload_len;
    if (body_len > SIZE_MAX - sizeof(cmq_frame_hdr_t)) return NULL;
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
    if (payload_len > 0 && payload) memcpy(p, payload, payload_len);

    size_t len = cmq_frame_encode(buf, buf_size, CMQ_OP_MESSAGE, 0,
                                   buf + sizeof(cmq_frame_hdr_t), body_len);
    if (len == 0) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

typedef struct {
    cmq_client_t *client;
    uint32_t sub_id;
    char subject[CMQ_MAX_SUBJECT];
    char queue_group[CMQ_MAX_QUEUE_GROUP];
} cmq_sub_ref_t;

/* Value snapshot for async (coroutine) delivery — no live ref/client pointers. */
typedef struct {
    uint32_t client_id;
    uint32_t conn_gen;
    int worker_id;
    uint32_t sub_id;
    uint32_t account_epoch; /* subscriber CONNECT epoch — not get() current */
    char subject[CMQ_MAX_SUBJECT];
    char queue_group[CMQ_MAX_QUEUE_GROUP];
    char account_name[CMQ_ACCOUNT_NAME_SIZE];
} cmq_deliver_tgt_t;

static void cmq_fill_deliver_tgt(cmq_deliver_tgt_t *t, const cmq_sub_ref_t *ref) {
    t->client_id = ref->client->id;
    t->conn_gen = ref->client->conn_gen;
    t->worker_id = ref->client->worker_id;
    t->sub_id = ref->sub_id;
    t->account_epoch = ref->client->account_epoch;
    memcpy(t->subject, ref->subject, CMQ_MAX_SUBJECT);
    memcpy(t->queue_group, ref->queue_group, CMQ_MAX_QUEUE_GROUP);
    memcpy(t->account_name, ref->client->account_name, CMQ_ACCOUNT_NAME_SIZE);
}

/* Build a queue-group-deduped target list from match results.
   Queue groups (scoped by subscription subject) pick one member via
   round-robin (srv->qg_rr_counter). Must hold sublist_lock (rd).
   Returns malloc'd array; *out_n = count. OOM: NULL + *out_n = SIZE_MAX.
   Empty/filtered: NULL + *out_n = 0. Caller frees. */
static cmq_deliver_tgt_t *snapshot_deliver_targets(cmq_server_t *srv,
                                                    cmq_sublist_result_t *result,
                                                    size_t *out_n) {
    *out_n = 0;
    if (result->count == 0) return NULL;
    if (result->count > SIZE_MAX / sizeof(cmq_deliver_tgt_t) ||
        result->count > SIZE_MAX / sizeof(size_t)) {
        *out_n = SIZE_MAX;
        return NULL;
    }
    cmq_deliver_tgt_t *tgts = malloc(result->count * sizeof(cmq_deliver_tgt_t));
    uint8_t *used = calloc(result->count, 1);
    size_t *memb = malloc(result->count * sizeof(size_t));
    if (!tgts || !used || !memb) {
        free(tgts);
        free(used);
        free(memb);
        *out_n = SIZE_MAX;
        return NULL;
    }

    size_t n = 0;
    for (size_t i = 0; i < result->count; i++) {
        if (used[i]) continue;
        cmq_sub_ref_t *ref = (cmq_sub_ref_t *)result->entries[i];
        if (!ref || !ref->client) continue;
        /* Do not read client->state here (cross-thread race); send paths filter. */

        if (ref->queue_group[0] == '\0') {
            /* Same epoch filter as QG: skip soft-deleted holders early. */
            if (!client_account_live(srv, ref->client))
                continue;
            cmq_fill_deliver_tgt(&tgts[n++], ref);
            continue;
        }

        /* Collect live members of this (account, subject, queue_group).
           Skip epoch-dead holders so RR does not pick a member that send
           paths will drop (one pick, no QG retry). */
        size_t mn = 0;
        for (size_t j = i; j < result->count; j++) {
            if (used[j]) continue;
            cmq_sub_ref_t *rj = (cmq_sub_ref_t *)result->entries[j];
            if (!rj || !rj->client) continue;
            if (rj->queue_group[0] == '\0') continue;
            if (strcmp(rj->queue_group, ref->queue_group) != 0) continue;
            if (strcmp(rj->subject, ref->subject) != 0) continue;
            if (strcmp(rj->client->account_name, ref->client->account_name) != 0)
                continue;
            used[j] = 1;
            if (!client_account_live(srv, rj->client))
                continue;
            memb[mn++] = j;
        }
        if (mn == 0) continue;
        uint64_t tick = cmq_atomic_fetch_add_u64(&srv->qg_rr_counter, 1,
                                                  CMQ_ATOMIC_RELAXED);
        size_t pick = (size_t)(tick % (uint64_t)mn);
        cmq_fill_deliver_tgt(&tgts[n++],
                             (cmq_sub_ref_t *)result->entries[memb[pick]]);
    }
    free(used);
    free(memb);
    *out_n = n;
    return tgts;
}

/* pub_account non-empty: recheck may_deliver (same as mailbox SEND). */
static int deliver_acl_ok(cmq_server_t *srv, const char *pub_account,
                           const char *sub_account,
                           const uint8_t *frame, size_t flen) {
    if (!pub_account || !pub_account[0] || !srv || !srv->accounts)
        return 1;
    if (!sub_account) return 0;
    char subj[CMQ_MAX_SUBJECT];
    if (message_frame_subject(frame, flen, subj, sizeof(subj)) != 0)
        return 0;
    return cmq_account_may_deliver(srv->accounts, pub_account, sub_account,
                                    subj);
}

/* Lookup by id+gen and send. Worker targets: owning thread may send_local;
   foreign threads only mailbox-push (never touch subs/write_buf off-owner). */
static int client_send_by_id(cmq_server_t *srv, int worker_id, uint32_t client_id,
                              uint32_t conn_gen, uint32_t require_sub_id,
                              const uint8_t *frame, size_t flen,
                              const char *pub_account) {
    if (worker_id >= 0 && srv->workers && worker_id < srv->num_workers) {
        cmq_worker_t *w = &srv->workers[worker_id];
        if (cmq_current_worker_id != worker_id) {
            if (worker_push_msg(w, client_id, conn_gen, frame, flen,
                                 require_sub_id, 0, NULL, 0, 0, NULL, 0,
                                 pub_account, 0) == 0)
                return 1;
            /* Queue full — nudge EOF; do not race write_buf/subs cross-thread. */
            cmq_mutex_lock(&w->clients_lock);
            cmq_client_t *c = cmq_idmap_get(w->idmap, client_id);
            if (c && c->conn_gen == conn_gen && c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            cmq_mutex_unlock(&w->clients_lock);
            return 0;
        }
        cmq_mutex_lock(&w->clients_lock);
        cmq_client_t *c = cmq_idmap_get(w->idmap, client_id);
        int do_send = 0;
        int dead_acct = 0;
        if (c && c->conn_gen == conn_gen && c->state == CMQ_CLIENT_CONNECTED &&
            client_account_live(srv, c) &&
            (require_sub_id == 0 || client_has_sub(c, require_sub_id))) {
            do_send = 1;
        } else if (c && c->conn_gen == conn_gen &&
                   c->state == CMQ_CLIENT_CONNECTED &&
                   !client_account_live(srv, c)) {
            c->state = CMQ_CLIENT_CLOSING;
            dead_acct = 1;
        }
        cmq_mutex_unlock(&w->clients_lock);
        /* Owning thread: send after unlock (same as mailbox drain).
           Re-check account/ACL after unlock — soft-delete / revoke TOCTOU. */
        int ok = 0;
        if (do_send && c) {
            if (!client_account_live(srv, c)) {
                cmq_mutex_lock(&w->clients_lock);
                if (c->conn_gen == conn_gen && c->state == CMQ_CLIENT_CONNECTED)
                    c->state = CMQ_CLIENT_CLOSING;
                cmq_mutex_unlock(&w->clients_lock);
                dead_acct = 1;
            } else if (!deliver_acl_ok(srv, pub_account, c->account_name,
                                        frame, flen)) {
                /* ACL revoked mid-flight — drop. */
            } else if (cmq_client_send_local(c, frame, flen) == 0) {
                ok = 1;
            }
        }
        /* Soft-deleted account: drop ghost subs promptly via worker teardown. */
        if (dead_acct)
            worker_teardown_or_shutdown(w, client_id, conn_gen);
        return ok;
    }
    cmq_mutex_lock(&srv->clients_lock);
    cmq_client_t *c = cmq_idmap_get(srv->idmap, client_id);
    int ok = 0;
    int dead_acct = 0;
    int dead_fd = -1;
    if (c && c->conn_gen == conn_gen && c->state == CMQ_CLIENT_CONNECTED &&
        client_account_live(srv, c) &&
        (require_sub_id == 0 || client_has_sub(c, require_sub_id))) {
        /* May run off acceptor thread (worker deliver to acceptor-owned) —
           hold clients_lock across send so finish_closing cannot race. */
        if (deliver_acl_ok(srv, pub_account, c->account_name, frame, flen) &&
            cmq_client_send_local(c, frame, flen) == 0)
            ok = 1;
    } else if (c && c->conn_gen == conn_gen &&
               c->state == CMQ_CLIENT_CONNECTED &&
               !client_account_live(srv, c)) {
        /* Acceptor-owned: mark CLOSING; owning loop tears down after EOF. */
        c->state = CMQ_CLIENT_CLOSING;
        dead_acct = 1;
        dead_fd = c->fd;
    }
    cmq_mutex_unlock(&srv->clients_lock);
    if (dead_acct) {
        if (dead_fd >= 0)
            (void)shutdown(dead_fd, SHUT_RDWR);
        if (srv->ev_loop)
            cmq_ev_wakeup(srv->ev_loop);
    }
    return ok;
}

/* Queue to worker mailbox; same-worker prefers send_local first (honest
   REQUEST PUBACK / msgs_out). Cross-worker briefly yields and retries so a
   full queue can drain without silent drop.
   Returns 1 if send_local completed, 2 if only queued (msgs_out deferred), 0 fail. */
static int deliver_via_worker(cmq_server_t *srv, int worker_id,
                               uint32_t client_id, uint32_t conn_gen,
                               uint32_t sub_id,
                               const uint8_t *frame, size_t flen,
                               int coro_ok,
                               const char *account_name, uint32_t account_epoch,
                               uint32_t payload_bytes,
                               const char *pub_account) {
    cmq_worker_t *w = &srv->workers[worker_id];
    if (cmq_current_worker_id == worker_id) {
        int r = client_send_by_id(srv, worker_id, client_id, conn_gen, sub_id,
                                   frame, flen, pub_account);
        if (r) return 1;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        if (worker_push_msg(w, client_id, conn_gen, frame, flen, sub_id,
                             1, account_name, account_epoch, payload_bytes,
                             NULL, 0, pub_account, 0) == 0)
            return 2;
        if (cmq_current_worker_id == worker_id)
            return client_send_by_id(srv, worker_id, client_id, conn_gen, sub_id,
                                     frame, flen, pub_account);
        if (attempt + 1 >= 4)
            break;
        if (coro_ok)
            cmq_coro_yield();
        else {
            struct timespec ts = {0, 50000L}; /* 50µs */
            nanosleep(&ts, NULL);
        }
    }
    return 0;
}

/* Inbox payloads must not fan out to '>' / other wildcards (sniff). */
static int deliver_tgt_accepts_subject(const cmq_deliver_tgt_t *t,
                                        const char *subject) {
    if (!t || !subject) return 0;
    if (strncmp(subject, "_INBOX.", 7) == 0)
        return strcmp(t->subject, subject) == 0;
    return 1;
}

/* Sync fan-out by stable client_id — no sublist lock held (teardown-safe).
   Returns 0 if at least one delivery was queued/sent, -1 on total failure. */
static int deliver_targets_sync(cmq_server_t *srv,
                                  cmq_deliver_tgt_t *tgts, size_t ntgt,
                                  const char *subject,
                                  const char *pub_account,
                                  const uint8_t *payload, size_t payload_len,
                                  const uint8_t *headers, size_t headers_len) {
    if (!tgts || ntgt == 0) return -1;
    size_t flen = 0;
    uint8_t *frame = cmq_build_message_frame(0, subject, payload, payload_len,
                                              headers, headers_len, &flen);
    if (!frame) return -1;

    int any = 0;
    for (size_t i = 0; i < ntgt; i++) {
        cmq_deliver_tgt_t *t = &tgts[i];
        if (!deliver_tgt_accepts_subject(t, subject))
            continue;
        if (!cmq_account_may_deliver(srv->accounts, pub_account,
                                      t->account_name, subject))
            continue;
        cmq_patch_message_sub_id(frame, t->sub_id);
        int delivered = 0;
        if (t->worker_id >= 0 && srv->workers &&
            t->worker_id < srv->num_workers) {
            delivered = deliver_via_worker(srv, t->worker_id, t->client_id,
                                            t->conn_gen, t->sub_id, frame, flen, 0,
                                            t->account_name, t->account_epoch,
                                            (uint32_t)payload_len, pub_account);
        } else {
            delivered = client_send_by_id(srv, -1, t->client_id, t->conn_gen,
                                           t->sub_id, frame, flen, pub_account);
        }
        if (delivered) {
            any = 1;
            /* 2 = mailbox queued — credit msgs_out after send_local. */
            if (delivered == 1) {
                cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1,
                                          CMQ_ATOMIC_RELAXED);
                cmq_account_t *oacc =
                    cmq_account_get(srv->accounts, t->account_name, NULL);
                if (oacc) {
                    cmq_account_inc_msgs_out(oacc, t->account_epoch,
                                             (uint64_t)payload_len);
                    cmq_account_release(srv->accounts, oacc);
                }
            }
        } else {
            cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
        }
    }
    free(frame);
    return any ? 0 : -1;
}

/* Same-worker / acceptor REQUEST send: buffer then drain so PUBACK means on-wire. */
static int client_send_by_id_drain(cmq_server_t *srv, int worker_id,
                                    uint32_t client_id, uint32_t conn_gen,
                                    uint32_t require_sub_id,
                                    const uint8_t *frame, size_t flen,
                                    const char *pub_account) {
    if (worker_id >= 0 && srv->workers && worker_id < srv->num_workers) {
        if (cmq_current_worker_id != worker_id)
            return 0;
        cmq_worker_t *w = &srv->workers[worker_id];
        cmq_mutex_lock(&w->clients_lock);
        cmq_client_t *c = cmq_idmap_get(w->idmap, client_id);
        int do_send = 0;
        if (c && c->conn_gen == conn_gen && c->state == CMQ_CLIENT_CONNECTED &&
            client_account_live(srv, c) &&
            (require_sub_id == 0 || client_has_sub(c, require_sub_id)))
            do_send = 1;
        cmq_mutex_unlock(&w->clients_lock);
        if (!do_send || !c) return 0;
        if (!client_account_live(srv, c)) return 0;
        if (!deliver_acl_ok(srv, pub_account, c->account_name, frame, flen))
            return 0;
        if (cmq_client_send_local(c, frame, flen) != 0) return 0;
        return client_drain_write_sync(c) == 0 ? 1 : 0;
    }
    /* Acceptor-owned: hold clients_lock across send+drain (no bare-c* UAF). */
    cmq_mutex_lock(&srv->clients_lock);
    cmq_client_t *c = cmq_idmap_get(srv->idmap, client_id);
    int ok = 0;
    if (c && c->conn_gen == conn_gen && c->state == CMQ_CLIENT_CONNECTED &&
        client_account_live(srv, c) &&
        (require_sub_id == 0 || client_has_sub(c, require_sub_id)) &&
        deliver_acl_ok(srv, pub_account, c->account_name, frame, flen) &&
        cmq_client_send_local(c, frame, flen) == 0 &&
        client_drain_write_sync(c) == 0)
        ok = 1;
    cmq_mutex_unlock(&srv->clients_lock);
    return ok;
}

/* Cross-worker REQUEST: mailbox + wait for owner send_local(+drain).
   Heap refcounted sync slot — publisher may abandon without UAF/livelock.
   While waiting, drain our own SEND mailbox so A↔B sync REQUESTs cannot
   deadlock two worker event threads. */
static int deliver_request_via_worker(cmq_server_t *srv, int worker_id,
                                       uint32_t client_id, uint32_t conn_gen,
                                       uint32_t sub_id, const uint8_t *frame,
                                       size_t flen, const char *pub_account,
                                       cmq_client_t *publisher) {
    cmq_worker_t *w = &srv->workers[worker_id];
    cmq_worker_t *self =
        (cmq_current_worker_id >= 0 && srv->workers &&
         cmq_current_worker_id < srv->num_workers)
            ? &srv->workers[cmq_current_worker_id]
            : NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        cmq_req_sync_t *sync = calloc(1, sizeof(*sync));
        if (!sync) break;
        sync->refs = 2;
        if (worker_push_msg(w, client_id, conn_gen, frame, flen, sub_id,
                             0, NULL, 0, 0, &sync->result, 1, pub_account,
                             1) != 0) {
            free(sync);
            if (attempt + 1 >= 4) break;
            if (self) worker_drain_sends(self, CMQ_WORKER_WAKE_BATCH);
            if (publisher) client_touch_activity(publisher);
            struct timespec ts = {0, 50000L};
            nanosleep(&ts, NULL);
            continue;
        }
        int waited = 0;
        for (;;) {
            int v = __atomic_load_n(&sync->result, __ATOMIC_ACQUIRE);
            if (req_sync_terminal(v)) {
                req_sync_release(sync);
                return v == 1 ? 1 : 0;
            }
            if (self) worker_drain_sends(self, CMQ_WORKER_WAKE_BATCH);
            if (publisher) client_touch_activity(publisher);
            if (waited >= 100000) {
                /* ~5s soft deadline: cancel only while still queued. */
                cmq_mutex_lock(&w->msg_lock);
                cmq_worker_msg_t **pp = &w->msg_head;
                cmq_worker_msg_t *prev = NULL;
                cmq_worker_msg_t *found = NULL;
                while (*pp) {
                    cmq_worker_msg_t *m = *pp;
                    if (m->sync_result == &sync->result) {
                        *pp = m->next;
                        if (w->msg_tail == m)
                            w->msg_tail = prev;
                        if (w->msg_pending > 0)
                            w->msg_pending--;
                        m->next = NULL;
                        found = m;
                        break;
                    }
                    prev = m;
                    pp = &m->next;
                }
                cmq_mutex_unlock(&w->msg_lock);
                if (found) {
                    free(found->buf);
                    free(found);
                    free(sync); /* never handed to worker complete */
                    return 0;
                }
                /* In-flight — wait drain bound, nudge EOF, then hard-abandon. */
                for (int extra = 0; extra < 50000; extra++) { /* ~2.5s */
                    int v2 = __atomic_load_n(&sync->result, __ATOMIC_ACQUIRE);
                    if (req_sync_terminal(v2)) {
                        req_sync_release(sync);
                        return v2 == 1 ? 1 : 0;
                    }
                    if (self) worker_drain_sends(self, CMQ_WORKER_WAKE_BATCH);
                    if (publisher) client_touch_activity(publisher);
                    if (extra == 40000) {
                        cmq_mutex_lock(&w->clients_lock);
                        cmq_client_t *t = cmq_idmap_get(w->idmap, client_id);
                        if (t && t->conn_gen == conn_gen && t->fd >= 0)
                            (void)shutdown(t->fd, SHUT_RDWR);
                        cmq_mutex_unlock(&w->clients_lock);
                    }
                    struct timespec ts2 = {0, 50000L};
                    nanosleep(&ts2, NULL);
                }
                /* Hard abandon: CAS pending→fail; if claimed (2), wait final. */
                for (int hard = 0; hard < 50000; hard++) { /* ~2.5s more */
                    int v2 = __atomic_load_n(&sync->result, __ATOMIC_ACQUIRE);
                    if (req_sync_terminal(v2)) {
                        req_sync_release(sync);
                        return v2 == 1 ? 1 : 0;
                    }
                    if (v2 == 0) {
                        int expected = 0;
                        if (__atomic_compare_exchange_n(
                                &sync->result, &expected, -1, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                            req_sync_release(sync);
                            return 0;
                        }
                        continue;
                    }
                    /* Claimed (2) — wait for worker to resolve. */
                    if (self) worker_drain_sends(self, CMQ_WORKER_WAKE_BATCH);
                    if (publisher) client_touch_activity(publisher);
                    struct timespec ts2 = {0, 50000L};
                    nanosleep(&ts2, NULL);
                }
                /* Still unresolved: force fail; claimed worker will not clobber. */
                __atomic_store_n(&sync->result, -1, __ATOMIC_RELEASE);
                req_sync_release(sync);
                return 0;
            }
            struct timespec ts = {0, 50000L};
            nanosleep(&ts, NULL);
            waited++;
        }
    }
    return 0;
}

/* REQUEST fan-out by client_id after sublist unlock (same safety as publish).
   Returns number of successful on-wire deliveries (for honest PUBACK). */
static size_t deliver_request_targets(cmq_server_t *srv,
                                     cmq_deliver_tgt_t *tgts, size_t ntgt,
                                     const char *subject, const char *reply_to,
                                     const char *pub_account,
                                     const uint8_t *payload, size_t payload_len,
                                     cmq_client_t *publisher) {
    if (!tgts || ntgt == 0) return 0;
    size_t flen = 0;
    uint8_t *frame = cmq_build_request_message_frame(0, subject, reply_to,
                                                      payload, payload_len, &flen);
    if (!frame) return 0;

    size_t delivered_n = 0;
    for (size_t i = 0; i < ntgt; i++) {
        cmq_deliver_tgt_t *t = &tgts[i];
        if (!deliver_tgt_accepts_subject(t, subject))
            continue;
        if (!cmq_account_may_deliver(srv->accounts, pub_account,
                                      t->account_name, subject))
            continue;
        cmq_patch_message_sub_id(frame, t->sub_id);
        int delivered = 0;
        if (t->worker_id >= 0 && srv->workers &&
            t->worker_id < srv->num_workers) {
            if (cmq_current_worker_id == t->worker_id)
                delivered = client_send_by_id_drain(srv, t->worker_id,
                                                     t->client_id, t->conn_gen,
                                                     t->sub_id, frame, flen,
                                                     pub_account);
            else
                delivered = deliver_request_via_worker(
                    srv, t->worker_id, t->client_id, t->conn_gen, t->sub_id,
                    frame, flen, pub_account, publisher);
        } else {
            delivered = client_send_by_id_drain(srv, -1, t->client_id,
                                                 t->conn_gen, t->sub_id,
                                                 frame, flen, pub_account);
        }
        if (delivered) {
            delivered_n++;
            cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1,
                                      CMQ_ATOMIC_RELAXED);
            cmq_account_t *oacc =
                cmq_account_get(srv->accounts, t->account_name, NULL);
            if (oacc) {
                cmq_account_inc_msgs_out(oacc, t->account_epoch,
                                         (uint64_t)payload_len);
                cmq_account_release(srv->accounts, oacc);
            }
        } else {
            cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
        }
    }
    free(frame);
    return delivered_n;
}

typedef struct {
    cmq_server_t *srv;
    cmq_deliver_tgt_t *targets;
    size_t target_count;
    char subject[CMQ_MAX_SUBJECT];
    char pub_account[CMQ_ACCOUNT_NAME_SIZE];
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *headers;
    size_t headers_len;
    uint8_t *frame;                 /* MESSAGE template; sub_id patched per target */
    size_t frame_len;
    size_t idx;
    cmq_coro_t *coro;
    uint32_t publisher_id;          /* 0 = no publisher ERROR notify */
    uint32_t publisher_gen;
    int publisher_worker_id;
    int delivered_any;
    int suppress_fail_error;        /* 1 if cluster already accepted the msg */
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
        if (!deliver_tgt_accepts_subject(t, ctx->subject))
            continue;
        if (!cmq_account_may_deliver(srv->accounts, ctx->pub_account,
                                      t->account_name, ctx->subject))
            continue;
        cmq_patch_message_sub_id(ctx->frame, t->sub_id);

        int delivered = 0;
        /* Never hold a bare client* across unlock — route by stable id.
           Queue raw CMQ; owning thread wraps WS in send_local. */
        if (t->worker_id >= 0 && srv->workers &&
            t->worker_id < srv->num_workers) {
            delivered = deliver_via_worker(srv, t->worker_id, t->client_id,
                                            t->conn_gen, t->sub_id, ctx->frame,
                                            ctx->frame_len, 1,
                                            t->account_name, t->account_epoch,
                                            (uint32_t)ctx->payload_len,
                                            ctx->pub_account);
        } else {
            delivered = client_send_by_id(srv, -1, t->client_id, t->conn_gen,
                                           t->sub_id, ctx->frame, ctx->frame_len,
                                           ctx->pub_account);
        }

        if (delivered) {
            ctx->delivered_any = 1;
            if (delivered == 1) {
                cmq_atomic_fetch_add_u64(&srv->stat_messages_out, 1,
                                          CMQ_ATOMIC_RELAXED);
                cmq_account_t *oacc =
                    cmq_account_get(srv->accounts, t->account_name, NULL);
                if (oacc) {
                    cmq_account_inc_msgs_out(oacc, t->account_epoch,
                                             (uint64_t)ctx->payload_len);
                    cmq_account_release(srv->accounts, oacc);
                }
            }
        } else {
            cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
        }

        batch_count++;
        if (batch_count >= CMQ_CORO_DELIVER_BATCH) {
            ctx->idx = i + 1;
            cmq_coro_yield();
            batch_count = 0;
        }
    }

done:
    if (!ctx->delivered_any && ctx->publisher_id != 0 &&
        !ctx->suppress_fail_error) {
        static const char emsg[] = "delivery failed";
        uint8_t ebuf[256];
        size_t elen = cmq_frame_encode(ebuf, sizeof(ebuf), CMQ_OP_ERROR, 0,
                                        (const uint8_t *)emsg, sizeof(emsg) - 1);
        if (elen > 0) {
            if (ctx->publisher_worker_id >= 0 && srv->workers &&
                ctx->publisher_worker_id < srv->num_workers) {
                cmq_worker_t *pw = &srv->workers[ctx->publisher_worker_id];
                if (worker_push_msg(pw, ctx->publisher_id, ctx->publisher_gen,
                                     ebuf, elen, 0, 0, NULL, 0, 0, NULL, 0,
                                     NULL, 0) != 0) {
                    /* Queue full — sync send or force EOF so publisher notices. */
                    if (client_send_by_id(srv, ctx->publisher_worker_id,
                                           ctx->publisher_id, ctx->publisher_gen,
                                           0, ebuf, elen, NULL) != 0) {
                        cmq_mutex_lock(&pw->clients_lock);
                        cmq_client_t *pub =
                            cmq_idmap_get(pw->idmap, ctx->publisher_id);
                        if (pub && pub->conn_gen == ctx->publisher_gen &&
                            pub->fd >= 0)
                            shutdown(pub->fd, SHUT_RDWR);
                        cmq_mutex_unlock(&pw->clients_lock);
                    }
                }
            } else {
                (void)client_send_by_id(srv, -1, ctx->publisher_id,
                                         ctx->publisher_gen, 0, ebuf, elen,
                                         NULL);
            }
        }
    }
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

static void deliver_ctx_free(void *arg) {
    cmq_deliver_ctx_t *ctx = (cmq_deliver_ctx_t *)arg;
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
   Takes ownership of targets/payload/headers on success; frees on failure.
   Returns 0 on success, -1 on OOM (caller should ERROR / sync-fallback). */
static int worker_coro_spawn_deliver(cmq_worker_t *w,
                                       cmq_server_t *srv,
                                       cmq_deliver_tgt_t *targets,
                                       size_t target_count,
                                       const char *subject,
                                       const char *pub_account,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       const uint8_t *headers,
                                       size_t headers_len,
                                       uint32_t publisher_id,
                                       uint32_t publisher_gen,
                                       int publisher_worker_id,
                                       int suppress_fail_error) {
    /* Sync fan-out when coro pool is full or coro alloc fails.
       Deliver every target — do not silently truncate large fan-outs. */
    #define CMQ_CORO_SYNC_FALLBACK(tgts, n, subj, pub, pay, plen, hdr, hlen) do { \
        int _rc = deliver_targets_sync(srv, (tgts), (n), (subj), (pub), \
                                        (pay), (plen), (hdr), (hlen)); \
        free(tgts); \
        free((void *)(pay)); \
        if (hdr) free((void *)(hdr)); \
        return _rc; \
    } while (0)

    cmq_deliver_ctx_t *ctx = calloc(1, sizeof(cmq_deliver_ctx_t));
    if (!ctx) {
        CMQ_CORO_SYNC_FALLBACK(targets, target_count, subject, pub_account,
                               payload, payload_len, headers, headers_len);
    }
    ctx->srv = srv;
    ctx->targets = targets;
    ctx->target_count = target_count;
    strncpy(ctx->subject, subject, CMQ_MAX_SUBJECT - 1);
    if (pub_account)
        strncpy(ctx->pub_account, pub_account, CMQ_ACCOUNT_NAME_SIZE - 1);
    ctx->payload = payload;
    ctx->payload_len = payload_len;
    ctx->headers = headers;
    ctx->headers_len = headers_len;
    ctx->idx = 0;
    ctx->publisher_id = publisher_id;
    ctx->publisher_gen = publisher_gen;
    ctx->publisher_worker_id = publisher_worker_id;
    ctx->suppress_fail_error = suppress_fail_error;

    cmq_coro_t *coro = cmq_coro_create(deliver_coro_func, ctx, 32768);
    if (!coro) {
        /* Steal owned buffers out of ctx so sync path can free them once. */
        cmq_deliver_tgt_t *tgts = ctx->targets;
        size_t n = ctx->target_count;
        const uint8_t *pay = ctx->payload;
        size_t plen = ctx->payload_len;
        const uint8_t *hdr = ctx->headers;
        size_t hlen = ctx->headers_len;
        char subj[CMQ_MAX_SUBJECT];
        char pub[CMQ_ACCOUNT_NAME_SIZE];
        memcpy(subj, ctx->subject, sizeof(subj));
        memcpy(pub, ctx->pub_account, sizeof(pub));
        free(ctx);
        CMQ_CORO_SYNC_FALLBACK(tgts, n, subj, pub[0] ? pub : NULL,
                               pay, plen, hdr, hlen);
    }
    ctx->coro = coro;

    if (w->coro_pool && w->coro_count < w->coro_cap) {
        w->coro_pool[w->coro_count++] = coro;
    } else {
        /* Pool full: sync fan-out for all targets (rare path). */
        cmq_coro_destroy(coro);
        int rc = deliver_targets_sync(srv, ctx->targets, ctx->target_count,
                                       ctx->subject, ctx->pub_account,
                                       ctx->payload, ctx->payload_len,
                                       ctx->headers, ctx->headers_len);
        deliver_ctx_free(ctx);
        return rc;
    }
    return 0;
#undef CMQ_CORO_SYNC_FALLBACK
}

/* Forward a framed op to cluster routes (heap-sized encode).
   Returns 0 on encode+broadcast attempt, -1 on OOM / encode failure.
   *out_sent (optional) receives peers that accepted the full write.
   EAGAIN peers bump stat_messages_dropped and do not count as sent. */
static int cmq_route_forward_op(cmq_server_t *srv, cmq_op_t op, uint8_t flags,
                                 const uint8_t *payload, size_t payload_len,
                                 size_t *out_sent) {
    if (out_sent) *out_sent = 0;
    if (!srv || !srv->routes) return 0;
    if (payload_len > 0 && !payload) return 0;
    if (payload_len > SIZE_MAX - sizeof(cmq_frame_hdr_t)) return -1;
    size_t need = sizeof(cmq_frame_hdr_t) + payload_len;
    uint8_t *fwd = malloc(need);
    if (!fwd) {
        cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                  CMQ_ATOMIC_RELAXED);
        return -1;
    }
    size_t fwd_len = cmq_frame_encode(fwd, need, op, flags, payload, payload_len);
    if (fwd_len == 0) {
        free(fwd);
        cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                  CMQ_ATOMIC_RELAXED);
        return -1;
    }
    size_t eagain = 0;
    size_t sent = cmq_route_broadcast(srv->routes, fwd, fwd_len, NULL, &eagain);
    free(fwd);
    if (out_sent) *out_sent = sent;
    if (eagain > 0) {
        cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, (uint64_t)eagain,
                                  CMQ_ATOMIC_RELAXED);
    }
    /* Any live peer deferred = incomplete fan-out (no retry queue yet). */
    if (eagain > 0)
        return -1;
    return 0;
}

/* True when configured cluster peers should have received this forward but
   none did — includes "routes configured but all dead" (align with BATCH).
   Partial fan-out (route_sent > 0 with some EAGAIN) is not a total miss.
   Staged-only peers (held, not mark_connected) do not receive broadcast —
   treat as miss so remote-only PUBLISH/BATCH is not silently dropped. */
static int cmq_route_forward_missed(cmq_server_t *srv, int route_rc,
                                    size_t route_sent) {
    if (route_rc != 0 && route_sent == 0) return 1;
    if (!srv || !srv->routes) return 0;
    size_t named = cmq_route_pool_count(srv->routes);
    size_t live = cmq_route_live_count(srv->routes);
    if (named > 0 && live == 0) return 1;
    if (live == 0) return 0;
    return route_sent == 0;
}

static void handle_publish(cmq_server_t *srv, cmq_client_t *c,
                            const cmq_frame_t *frame) {
    (void)c;
    if (!client_account_live(srv, c)) {
        cmq_send_error(c, "account inactive");
        c->state = CMQ_CLIENT_CLOSING;
        return;
    }
    if (!frame->payload || frame->payload_len < 2) {
        cmq_send_error(c, "invalid publish");
        return;
    }

    uint16_t subject_len = ((uint16_t)frame->payload[0] << 8) | frame->payload[1];
    if (subject_len == 0 || subject_len >= CMQ_MAX_SUBJECT) {
        cmq_send_error(c, "invalid subject");
        return;
    }
    if ((size_t)(2 + subject_len) > frame->payload_len) {
        cmq_send_error(c, "subject too long");
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + 2, subject_len);
    subject[subject_len] = '\0';
    if (!wire_cstr_exact(subject, subject_len) ||
        cmq_sublist_publish_subject_valid(subject) != 0) {
        cmq_send_error(c, "invalid subject");
        return;
    }

    /* Advance by wire subject_len. */
    size_t offset = 2 + (size_t)subject_len;
    if (offset + 2 <= frame->payload_len) {
        uint16_t reply_len = ((uint16_t)frame->payload[offset] << 8) |
                              frame->payload[offset + 1];
        if (offset + 2 + (size_t)reply_len > frame->payload_len) {
            cmq_send_error(c, "invalid publish");
            return;
        }
        if (reply_len > 0) {
            if (reply_len >= CMQ_MAX_SUBJECT) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
            char reply_to[CMQ_MAX_SUBJECT];
            memcpy(reply_to, frame->payload + offset + 2, reply_len);
            reply_to[reply_len] = '\0';
            if (!wire_cstr_exact(reply_to, reply_len) ||
                cmq_sublist_publish_subject_valid(reply_to) != 0) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
        }
        offset += 2 + (size_t)reply_len;
    }

    const uint8_t *headers = NULL;
    size_t headers_len = 0;
    if (frame->hdr.flags & CMQ_FLAG_HEADERS) {
        if (offset + 2 > frame->payload_len) {
            cmq_send_error(c, "invalid publish headers");
            return;
        }
        headers_len = ((uint16_t)frame->payload[offset] << 8) |
                       frame->payload[offset + 1];
        offset += 2;
        if (offset + headers_len > frame->payload_len) {
            cmq_send_error(c, "invalid publish headers");
            return;
        }
        headers = frame->payload + offset;
        offset += headers_len;
    }

    if (offset > frame->payload_len) {
        cmq_send_error(c, "invalid publish");
        return;
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

    if (!cmq_account_can_export(srv->accounts, c->account_name, subject)) {
        cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_error(c, "permission denied");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);

    cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, NULL);
    if (acc) {
        cmq_account_inc_msgs_in(acc, c->account_epoch, (uint64_t)msg_len);
        cmq_account_release(srv->accounts, acc);
    }

    size_t route_sent = 0;
    int route_rc = 0;

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    if (cmq_sublist_match(srv->sublist, subject, &result) != 0) {
        cmq_rwlock_unlock(&srv->sublist_lock);
        cmq_send_error(c, "delivery failed");
        return; /* OOM after validation — surface error, no silent drop */
    }

    if (result.count == 0) {
        cmq_sublist_result_free(&result);
        cmq_rwlock_unlock(&srv->sublist_lock);
        /* Remote-only: forward after local match succeeded (no OOM ghost). */
        if (!c->is_route)
            route_rc = cmq_route_forward_op(srv, CMQ_OP_PUBLISH, frame->hdr.flags,
                                             frame->payload, frame->payload_len,
                                             &route_sent);
        if (!c->is_route && cmq_route_forward_missed(srv, route_rc, route_sent))
            cmq_send_error(c, "route failed");
        return;
    }

    /* Always snapshot under the read lock, then unlock before any I/O. */
    size_t ntgt = 0;
    cmq_deliver_tgt_t *tgts = snapshot_deliver_targets(srv, &result, &ntgt);
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);
    if (ntgt == SIZE_MAX) {
        cmq_send_error(c, "delivery failed");
        return;
    }
    if (!tgts || ntgt == 0) {
        free(tgts);
        if (!c->is_route)
            route_rc = cmq_route_forward_op(srv, CMQ_OP_PUBLISH, frame->hdr.flags,
                                             frame->payload, frame->payload_len,
                                             &route_sent);
        if (!c->is_route && cmq_route_forward_missed(srv, route_rc, route_sent))
            cmq_send_error(c, "route failed");
        return;
    }

    /* Cluster forward only after local snapshot succeeded. */
    if (!c->is_route)
        route_rc = cmq_route_forward_op(srv, CMQ_OP_PUBLISH, frame->hdr.flags,
                                         frame->payload, frame->payload_len,
                                         &route_sent);

    if (ntgt > CMQ_CORO_DELIVER_BATCH && srv->num_workers > 0) {
        cmq_worker_t *w = &srv->workers[cmq_current_worker_id >= 0 ?
                                         cmq_current_worker_id : 0];
        uint8_t *coro_payload = malloc(msg_len ? msg_len : 1);
        uint8_t *coro_headers = NULL;
        if (!coro_payload) {
            free(tgts);
            cmq_send_error(c, "delivery failed");
            return;
        }
        if (msg_len > 0) memcpy(coro_payload, msg_payload, msg_len);
        if (headers_len > 0 && headers) {
            coro_headers = malloc(headers_len);
            if (!coro_headers) {
                free(coro_payload);
                free(tgts);
                cmq_send_error(c, "delivery failed");
                return;
            }
            memcpy(coro_headers, headers, headers_len);
        }

        if (worker_coro_spawn_deliver(w, srv, tgts, ntgt, subject,
                                       c->account_name,
                                       coro_payload, msg_len,
                                       coro_headers, headers_len,
                                       c->id, c->conn_gen, c->worker_id,
                                       route_sent > 0 ? 1 : 0) != 0) {
            cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                      CMQ_ATOMIC_RELAXED);
            if (route_sent == 0)
                cmq_send_error(c, "delivery failed");
        }
        return;
    }

    if (deliver_targets_sync(srv, tgts, ntgt, subject, c->account_name,
                              msg_payload, msg_len,
                              headers, headers_len) != 0) {
        cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                  CMQ_ATOMIC_RELAXED);
        /* Cluster already has the message — do not ERROR the publisher. */
        if (route_sent == 0)
            cmq_send_error(c, "delivery failed");
    }
    free(tgts);
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
    if (sub_id == 0) {
        cmq_send_suback(c, 0, 1);
        return;
    }
    uint16_t subject_len = ((uint16_t)frame->payload[4] << 8) |
                            frame->payload[5];
    if ((size_t)(6 + subject_len) > frame->payload_len ||
        subject_len == 0 || subject_len >= CMQ_MAX_SUBJECT) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + 6, subject_len);
    subject[subject_len] = '\0';
    if (!wire_cstr_exact(subject, subject_len) ||
        cmq_sublist_subject_valid(subject) != 0) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    /* Reject _INBOX wildcards — same-account _INBOX.> would steal replies. */
    if (strncmp(subject, "_INBOX.", 7) == 0 || strcmp(subject, "_INBOX") == 0) {
        for (const char *p = subject; *p; p++) {
            if (*p == '*' || *p == '>') {
                cmq_atomic_fetch_add_u64(&srv->stat_subscribes_rejected, 1,
                                          CMQ_ATOMIC_RELAXED);
                cmq_send_suback(c, sub_id, 1);
                return;
            }
        }
    }
    if (!cmq_account_can_import(srv->accounts, c->account_name, subject)) {
        cmq_atomic_fetch_add_u64(&srv->stat_subscribes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_suback(c, sub_id, 1);
        return;
    }

    char queue_group[CMQ_MAX_QUEUE_GROUP] = {0};
    size_t qg_offset = 6 + subject_len;
    if (qg_offset + 2 <= frame->payload_len) {
        uint16_t qg_len = ((uint16_t)frame->payload[qg_offset] << 8) |
                           frame->payload[qg_offset + 1];
        if (qg_len > 0) {
            if (qg_len >= CMQ_MAX_QUEUE_GROUP ||
                qg_offset + 2 + qg_len > frame->payload_len) {
                cmq_send_suback(c, sub_id, 1);
                return;
            }
            memcpy(queue_group, frame->payload + qg_offset + 2, qg_len);
            queue_group[qg_len] = '\0';
            if (!wire_cstr_exact(queue_group, qg_len)) {
                cmq_send_suback(c, sub_id, 1);
                return;
            }
        }
    }

    int sub_cap = srv->config.max_subs_per_client > 0
                      ? srv->config.max_subs_per_client
                      : CMQ_MAX_SUBS_PER_CLIENT;

    /* Locate existing sub_id but keep it until the replacement is inserted —
       otherwise OOM/insert failure would leave the client with no subscription. */
    cmq_sub_entry_t *old = NULL;
    cmq_sub_entry_t **pp = &c->subs;
    while (*pp) {
        if ((*pp)->sub_id == sub_id) {
            old = *pp;
            break;
        }
        pp = &(*pp)->next;
    }
    int replacing = (old != NULL);

    if (!replacing && c->sub_count >= sub_cap) {
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
    strncpy(ref->subject, subject, CMQ_MAX_SUBJECT - 1);
    ref->subject[CMQ_MAX_SUBJECT - 1] = '\0';
    strncpy(ref->queue_group, queue_group, CMQ_MAX_QUEUE_GROUP - 1);
    ref->queue_group[CMQ_MAX_QUEUE_GROUP - 1] = '\0';

    cmq_rwlock_wrlock(&srv->sublist_lock);
    /* Exact _INBOX.* is first-claim: another live client's exact sub wins.
       Soft-deleted (epoch-dead) holders do not block reclaim. */
    typedef struct {
        int worker_id;
        uint32_t client_id;
        uint32_t conn_gen;
    } cmq_inbox_dead_t;
    cmq_inbox_dead_t stack_deads[16];
    cmq_inbox_dead_t *deads = stack_deads;
    cmq_inbox_dead_t *heap_deads = NULL;
    size_t dead_cap = sizeof(stack_deads) / sizeof(stack_deads[0]);
    size_t ndead = 0;
    if (strncmp(subject, "_INBOX.", 7) == 0) {
        cmq_sublist_result_t claim;
        /* Fail-closed: match OOM must not skip first-claim (duplicate live
           _INBOX holders). */
        if (cmq_sublist_match(srv->sublist, subject, &claim) != 0) {
            cmq_rwlock_unlock(&srv->sublist_lock);
            free(ref);
            free(entry);
            cmq_atomic_fetch_add_u64(&srv->stat_subscribes_rejected, 1,
                                      CMQ_ATOMIC_RELAXED);
            cmq_send_suback(c, sub_id, 1);
            return;
        }
        if (claim.count > dead_cap &&
            claim.count <= SIZE_MAX / sizeof(*heap_deads)) {
            heap_deads = malloc(claim.count * sizeof(*heap_deads));
            if (heap_deads) {
                deads = heap_deads;
                dead_cap = claim.count;
            }
        }
        for (size_t i = 0; i < claim.count; i++) {
            cmq_sub_ref_t *cr = (cmq_sub_ref_t *)claim.entries[i];
            if (!cr || !cr->client) continue;
            if (strcmp(cr->subject, subject) != 0) continue;
            if (cr->client == c) continue;
            if (!client_account_live(srv, cr->client)) {
                if (ndead < dead_cap) {
                    deads[ndead].worker_id = cr->client->worker_id;
                    deads[ndead].client_id = cr->client->id;
                    deads[ndead].conn_gen = cr->client->conn_gen;
                    ndead++;
                } else {
                    /* Heap OOM: still nudge reclaim (no silent ghost skip). */
                    if (cr->client->worker_id >= 0 && srv->workers &&
                        cr->client->worker_id < srv->num_workers)
                        (void)worker_push_teardown(
                            &srv->workers[cr->client->worker_id],
                            cr->client->id, cr->client->conn_gen);
                    else {
                        /* Acceptor-owned: only SHUT_RDWR off-owner. */
                        cmq_mutex_lock(&srv->clients_lock);
                        if (cr->client->fd >= 0)
                            (void)shutdown(cr->client->fd, SHUT_RDWR);
                        cmq_mutex_unlock(&srv->clients_lock);
                        if (srv->ev_loop)
                            cmq_ev_wakeup(srv->ev_loop);
                    }
                }
                continue;
            }
            cmq_sublist_result_free(&claim);
            cmq_rwlock_unlock(&srv->sublist_lock);
            for (size_t di = 0; di < ndead; di++) {
                if (deads[di].worker_id >= 0 && srv->workers &&
                    deads[di].worker_id < srv->num_workers) {
                    cmq_worker_t *ww = &srv->workers[deads[di].worker_id];
                    if (worker_push_teardown(ww, deads[di].client_id,
                                              deads[di].conn_gen) != 0) {
                        cmq_mutex_lock(&ww->clients_lock);
                        for (int j = 0; j < ww->clients_count; j++) {
                            cmq_client_t *dc = ww->clients[j];
                            if (dc && dc->id == deads[di].client_id &&
                                dc->conn_gen == deads[di].conn_gen &&
                                dc->fd >= 0) {
                                shutdown(dc->fd, SHUT_RDWR);
                                break;
                            }
                        }
                        cmq_mutex_unlock(&ww->clients_lock);
                    }
                } else {
                    /* Acceptor-owned: foreign thread must not finish_closing /
                       touch acceptor watchers — nudge EOF for owning loop. */
                    cmq_mutex_lock(&srv->clients_lock);
                    cmq_client_t *ac =
                        cmq_idmap_get(srv->idmap, deads[di].client_id);
                    if (ac && ac->conn_gen == deads[di].conn_gen &&
                        ac->fd >= 0)
                        (void)shutdown(ac->fd, SHUT_RDWR);
                    cmq_mutex_unlock(&srv->clients_lock);
                    if (srv->ev_loop)
                        cmq_ev_wakeup(srv->ev_loop);
                }
            }
            free(heap_deads);
            free(ref);
            free(entry);
            cmq_atomic_fetch_add_u64(&srv->stat_subscribes_rejected, 1,
                                      CMQ_ATOMIC_RELAXED);
            cmq_send_suback(c, sub_id, 1);
            return;
        }
        cmq_sublist_result_free(&claim);
    }
    /* Remove old first so publish cannot briefly match both subjects. */
    cmq_sub_ref_t *old_ref = (old && old->ref) ? old->ref : NULL;
    char old_subject[CMQ_MAX_SUBJECT];
    int ghost_drop = 0;
    if (old_ref) {
        memcpy(old_subject, old->subject, CMQ_MAX_SUBJECT);
        cmq_sublist_remove(srv->sublist, old_subject, old_ref);
    }
    int irc = cmq_sublist_insert(srv->sublist, subject, ref);
    if (irc != 0 && old_ref) {
        /* Restore previous registration so the client keeps a live sub. */
        if (cmq_sublist_insert(srv->sublist, old_subject, old_ref) != 0) {
            free(old_ref);
            if (old) old->ref = NULL;
            old_ref = NULL;
            ghost_drop = 1;
        }
    } else if (irc == 0 && old_ref) {
        free(old_ref);
        old->ref = NULL;
        old_ref = NULL;
    }
    cmq_rwlock_unlock(&srv->sublist_lock);
    for (size_t di = 0; di < ndead; di++) {
        if (deads[di].worker_id >= 0 && srv->workers &&
            deads[di].worker_id < srv->num_workers) {
            cmq_worker_t *ww = &srv->workers[deads[di].worker_id];
            if (worker_push_teardown(ww, deads[di].client_id,
                                      deads[di].conn_gen) != 0) {
                cmq_mutex_lock(&ww->clients_lock);
                for (int j = 0; j < ww->clients_count; j++) {
                    cmq_client_t *dc = ww->clients[j];
                    if (dc && dc->id == deads[di].client_id &&
                        dc->conn_gen == deads[di].conn_gen && dc->fd >= 0) {
                        shutdown(dc->fd, SHUT_RDWR);
                        break;
                    }
                }
                cmq_mutex_unlock(&ww->clients_lock);
            }
        } else {
            cmq_mutex_lock(&srv->clients_lock);
            cmq_client_t *ac = cmq_idmap_get(srv->idmap, deads[di].client_id);
            if (ac && ac->conn_gen == deads[di].conn_gen && ac->fd >= 0)
                (void)shutdown(ac->fd, SHUT_RDWR);
            cmq_mutex_unlock(&srv->clients_lock);
            if (srv->ev_loop)
                cmq_ev_wakeup(srv->ev_loop);
        }
    }
    free(heap_deads);
    if (irc != 0) {
        free(ref);
        free(entry);
        if (ghost_drop && old) {
            cmq_sub_entry_t **q = &c->subs;
            while (*q) {
                if (*q == old) {
                    *q = old->next;
                    break;
                }
                q = &(*q)->next;
            }
            free(old);
            if (c->sub_count > 0) c->sub_count--;
            uint64_t cur = cmq_atomic_load_u64(&srv->stat_subscriptions,
                                                CMQ_ATOMIC_RELAXED);
            if (cur > 0)
                cmq_atomic_fetch_sub_u64(&srv->stat_subscriptions, 1,
                                          CMQ_ATOMIC_RELAXED);
            cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, NULL);
            if (a) {
                cmq_account_dec_subscriptions(a, c->account_epoch);
                cmq_account_release(srv->accounts, a);
            }
        }
        cmq_send_suback(c, sub_id, 1);
        return;
    }

    if (old) {
        cmq_sub_entry_t **q = &c->subs;
        while (*q) {
            if (*q == old) {
                *q = old->next;
                break;
            }
            q = &(*q)->next;
        }
        free(old);
    }

    entry->ref = ref;
    entry->next = c->subs;
    c->subs = entry;

    if (!replacing) {
        cmq_atomic_fetch_add_u64(&srv->stat_subscriptions, 1, CMQ_ATOMIC_RELAXED);
        c->sub_count++;
        cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, NULL);
        int credited =
            (a && cmq_account_inc_subscriptions(a, c->account_epoch) == 0);
        /* Soft-delete between live-check and credit: roll back ghost sub. */
        if (!credited || !client_account_live(srv, c)) {
            if (credited)
                cmq_account_dec_subscriptions(a, c->account_epoch);
            if (a) cmq_account_release(srv->accounts, a);
            c->subs = entry->next;
            if (c->sub_count > 0) c->sub_count--;
            uint64_t cur = cmq_atomic_load_u64(&srv->stat_subscriptions,
                                                CMQ_ATOMIC_RELAXED);
            if (cur > 0)
                cmq_atomic_fetch_sub_u64(&srv->stat_subscriptions, 1,
                                          CMQ_ATOMIC_RELAXED);
            cmq_rwlock_wrlock(&srv->sublist_lock);
            if (entry->ref) {
                cmq_sublist_remove(srv->sublist, entry->subject, entry->ref);
                free(entry->ref);
                entry->ref = NULL;
            }
            cmq_rwlock_unlock(&srv->sublist_lock);
            free(entry);
            cmq_send_suback(c, sub_id, 1);
            c->state = CMQ_CLIENT_CLOSING;
            return;
        }
        if (a) cmq_account_release(srv->accounts, a);
    } else if (!client_account_live(srv, c)) {
        /* Replace path: old entry already freed — roll back counts too or
           sub_count/stat/account quota stay inflated with zero live subs. */
        c->subs = entry->next;
        if (c->sub_count > 0) c->sub_count--;
        uint64_t cur = cmq_atomic_load_u64(&srv->stat_subscriptions,
                                            CMQ_ATOMIC_RELAXED);
        if (cur > 0)
            cmq_atomic_fetch_sub_u64(&srv->stat_subscriptions, 1,
                                      CMQ_ATOMIC_RELAXED);
        cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, NULL);
        if (a) {
            cmq_account_dec_subscriptions(a, c->account_epoch);
            cmq_account_release(srv->accounts, a);
        }
        cmq_rwlock_wrlock(&srv->sublist_lock);
        if (entry->ref) {
            cmq_sublist_remove(srv->sublist, entry->subject, entry->ref);
            free(entry->ref);
            entry->ref = NULL;
        }
        cmq_rwlock_unlock(&srv->sublist_lock);
        free(entry);
        cmq_send_suback(c, sub_id, 1);
        c->state = CMQ_CLIENT_CLOSING;
        return;
    }
    cmq_send_suback(c, sub_id, 0);
}

static void handle_unsubscribe(cmq_server_t *srv, cmq_client_t *c,
                                const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 4) {
        cmq_send_error(c, "invalid unsubscribe");
        return;
    }

    uint32_t sub_id = ((uint32_t)frame->payload[0] << 24) |
                      ((uint32_t)frame->payload[1] << 16) |
                      ((uint32_t)frame->payload[2] << 8) |
                      (uint32_t)frame->payload[3];

    int found = 0;
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
            {
                cmq_account_t *a = cmq_account_get(srv->accounts, c->account_name, NULL);
                if (a) {
                    cmq_account_dec_subscriptions(a, c->account_epoch);
                    cmq_account_release(srv->accounts, a);
                }
            }
            found = 1;
            break;
        }
        pp = &(*pp)->next;
    }

    if (!found) {
        cmq_send_error(c, "unknown subscription");
        return;
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
    uint16_t wire_subj = ((uint16_t)frame->payload[offset] << 8) |
                          frame->payload[offset + 1];
    offset += 2;
    if (wire_subj == 0 || wire_subj >= CMQ_MAX_SUBJECT ||
        offset + wire_subj > frame->payload_len) {
        cmq_send_error(c, "invalid subject");
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + offset, wire_subj);
    subject[wire_subj] = '\0';
    offset += wire_subj;
    if (!wire_cstr_exact(subject, wire_subj) ||
        cmq_sublist_publish_subject_valid(subject) != 0) {
        cmq_send_error(c, "invalid subject");
        return;
    }

    char reply_to[CMQ_MAX_SUBJECT] = {0};
    if (offset + 2 <= frame->payload_len) {
        uint16_t wire_reply = ((uint16_t)frame->payload[offset] << 8) |
                               frame->payload[offset + 1];
        if (offset + 2 + (size_t)wire_reply > frame->payload_len) {
            cmq_send_error(c, "invalid request");
            return;
        }
        offset += 2;
        if (wire_reply > 0) {
            if (wire_reply >= CMQ_MAX_SUBJECT) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
            memcpy(reply_to, frame->payload + offset, wire_reply);
            reply_to[wire_reply] = '\0';
            if (!wire_cstr_exact(reply_to, wire_reply) ||
                cmq_sublist_publish_subject_valid(reply_to) != 0) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
        }
        offset += wire_reply;
    }

    if (offset > frame->payload_len) {
        cmq_send_error(c, "invalid request");
        return;
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

    if (!cmq_account_can_export(srv->accounts, c->account_name, subject)) {
        cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_error(c, "permission denied");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);

    cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, NULL);
    if (acc) {
        cmq_account_inc_msgs_in(acc, c->account_epoch, (uint64_t)msg_len);
        cmq_account_release(srv->accounts, acc);
    }

    size_t route_sent = 0;
    int route_rc = 0;

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    if (cmq_sublist_match(srv->sublist, subject, &result) != 0) {
        cmq_rwlock_unlock(&srv->sublist_lock);
        cmq_send_error(c, "delivery failed");
        return;
    }
    size_t ntgt = 0;
    cmq_deliver_tgt_t *tgts = snapshot_deliver_targets(srv, &result, &ntgt);
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);

    if (ntgt == SIZE_MAX) {
        cmq_send_error(c, "delivery failed");
        return;
    }
    /* Only forward REQUEST when no local responders — otherwise peers
       also answer the same _INBOX and the client sees duplicate replies. */
    if (!c->is_route && ntgt == 0)
        route_rc = cmq_route_forward_op(srv, CMQ_OP_REQUEST, frame->hdr.flags,
                                         frame->payload, frame->payload_len,
                                         &route_sent);

    if (tgts && ntgt > 0) {
        size_t n = deliver_request_targets(srv, tgts, ntgt, subject, reply_to,
                                            c->account_name,
                                            msg_payload, msg_len, c);
        free(tgts);
        if (n > 0) {
            uint8_t ack[4] = {0};
            size_t ack_len = cmq_frame_encode(ack, sizeof(ack), CMQ_OP_PUBACK, 0, NULL, 0);
            if (ack_len > 0) cmq_client_send(c, ack, ack_len);
        } else {
            cmq_send_error(c, "delivery failed");
        }
    } else {
        free(tgts);
        if (!c->is_route && route_sent > 0) {
            uint8_t ack[4] = {0};
            size_t ack_len = cmq_frame_encode(ack, sizeof(ack), CMQ_OP_PUBACK, 0, NULL, 0);
            if (ack_len > 0) cmq_client_send(c, ack, ack_len);
        } else if (!c->is_route && cmq_route_forward_missed(srv, route_rc, route_sent)) {
            cmq_send_error(c, "route failed");
        } else if (!c->is_route) {
            cmq_send_error(c, "no responders");
        }
    }
}

static void handle_response(cmq_server_t *srv, cmq_client_t *c,
                             const cmq_frame_t *frame) {
    if (!frame->payload || frame->payload_len < 4) {
        cmq_send_error(c, "invalid response");
        return;
    }

    size_t offset = 0;
    uint16_t wire_subj = ((uint16_t)frame->payload[offset] << 8) |
                          frame->payload[offset + 1];
    offset += 2;
    if (wire_subj == 0 || wire_subj >= CMQ_MAX_SUBJECT ||
        offset + wire_subj > frame->payload_len) {
        cmq_send_error(c, "invalid response");
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + offset, wire_subj);
    subject[wire_subj] = '\0';
    offset += wire_subj;
    if (!wire_cstr_exact(subject, wire_subj) ||
        cmq_sublist_publish_subject_valid(subject) != 0) {
        cmq_send_error(c, "invalid subject");
        return;
    }

    if (offset > frame->payload_len) {
        cmq_send_error(c, "invalid response");
        return;
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

    /* Inbox replies are not published subjects — skip export ACL; may_deliver
       still gates cross-account delivery on the fan-out path. */
    if (strncmp(subject, "_INBOX.", 7) != 0 &&
        !cmq_account_can_export(srv->accounts, c->account_name, subject)) {
        cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                  CMQ_ATOMIC_RELAXED);
        cmq_send_error(c, "permission denied");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);
    cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, NULL);
    if (acc) {
        cmq_account_inc_msgs_in(acc, c->account_epoch, (uint64_t)msg_len);
        cmq_account_release(srv->accounts, acc);
    }

    size_t route_sent = 0;
    int route_rc = 0;

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    if (cmq_sublist_match(srv->sublist, subject, &result) != 0) {
        cmq_rwlock_unlock(&srv->sublist_lock);
        cmq_send_error(c, "delivery failed");
        return;
    }
    size_t ntgt = 0;
    cmq_deliver_tgt_t *tgts = snapshot_deliver_targets(srv, &result, &ntgt);
    cmq_sublist_result_free(&result);
    cmq_rwlock_unlock(&srv->sublist_lock);
    if (ntgt == SIZE_MAX) {
        cmq_send_error(c, "delivery failed");
        return;
    }
    /* Only forward RESPONSE when no local inbox — avoid broadcasting
       private _INBOX payloads to every cluster peer. */
    if (!c->is_route && ntgt == 0)
        route_rc = cmq_route_forward_op(srv, CMQ_OP_RESPONSE, frame->hdr.flags,
                                         frame->payload, frame->payload_len,
                                         &route_sent);

    if (tgts && ntgt > 0) {
        if (deliver_targets_sync(srv, tgts, ntgt, subject, c->account_name,
                                  msg_payload, msg_len, NULL, 0) != 0)
            cmq_send_error(c, "delivery failed");
    } else if (!c->is_route) {
        if (route_sent == 0) {
            if (cmq_route_forward_missed(srv, route_rc, route_sent))
                cmq_send_error(c, "route failed");
            else
                cmq_send_error(c, "no subscribers");
        }
    }
    free(tgts);
}

static void handle_stats(cmq_server_t *srv, cmq_client_t *c) {
    /* When auth is configured, only authenticated clients may read stats. */
    if (auth_configured(srv) && !c->username) {
        cmq_send_error(c, "unauthorized");
        return;
    }
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
    uint64_t drops = cmq_atomic_load_u64(&srv->stat_messages_dropped,
                                           CMQ_ATOMIC_RELAXED);

    uint64_t active64 = 0;
    cmq_mutex_lock(&srv->clients_lock);
    if (srv->clients_count > 0)
        active64 += (uint64_t)srv->clients_count;
    cmq_mutex_unlock(&srv->clients_lock);
    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_mutex_lock(&srv->workers[i].clients_lock);
            if (srv->workers[i].clients_count > 0)
                active64 += (uint64_t)srv->workers[i].clients_count;
            cmq_mutex_unlock(&srv->workers[i].clients_lock);
        }
    }
    uint32_t active = active64 > UINT32_MAX ? UINT32_MAX : (uint32_t)active64;

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
    payload[off++] = (uint8_t)((active >> 24) & 0xFF);
    payload[off++] = (uint8_t)((active >> 16) & 0xFF);
    payload[off++] = (uint8_t)((active >> 8) & 0xFF);
    payload[off++] = (uint8_t)(active & 0xFF);
    WRITE_U64(pub_rej);
    WRITE_U64(sub_rej);
    WRITE_U64(drops);
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
    if (count == 0 || count > CMQ_BATCH_MAX) {
        cmq_send_error(c, "invalid batch");
        return;
    }
    size_t offset = 2;

    /* Pass 1: validate the entire batch before any delivery (no partial fan-out). */
    for (uint16_t msg = 0; msg < count; msg++) {
        if (offset + 2 > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        uint16_t subject_len = ((uint16_t)frame->payload[offset] << 8) |
                                frame->payload[offset + 1];
        offset += 2;
        if (subject_len == 0 || subject_len >= CMQ_MAX_SUBJECT ||
            offset + subject_len > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        {
            char subj[CMQ_MAX_SUBJECT];
            memcpy(subj, frame->payload + offset, subject_len);
            subj[subject_len] = '\0';
            if (!wire_cstr_exact(subj, subject_len) ||
                cmq_sublist_publish_subject_valid(subj) != 0) {
                cmq_send_error(c, "invalid subject");
                return;
            }
            if (!cmq_account_can_export(srv->accounts, c->account_name, subj)) {
                cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                          CMQ_ATOMIC_RELAXED);
                cmq_send_error(c, "permission denied");
                return;
            }
        }
        offset += subject_len;

        if (offset + 2 > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        uint16_t reply_len = ((uint16_t)frame->payload[offset] << 8) |
                              frame->payload[offset + 1];
        if (offset + 2 + (size_t)reply_len > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        if (reply_len > 0) {
            if (reply_len >= CMQ_MAX_SUBJECT) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
            char reply_to[CMQ_MAX_SUBJECT];
            memcpy(reply_to, frame->payload + offset + 2, reply_len);
            reply_to[reply_len] = '\0';
            if (!wire_cstr_exact(reply_to, reply_len) ||
                cmq_sublist_publish_subject_valid(reply_to) != 0) {
                cmq_send_error(c, "invalid reply-to");
                return;
            }
        }
        offset += 2 + (size_t)reply_len;

        if (offset + 4 > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        uint32_t payload_len = ((uint32_t)frame->payload[offset] << 24) |
                                ((uint32_t)frame->payload[offset + 1] << 16) |
                                ((uint32_t)frame->payload[offset + 2] << 8) |
                                (uint32_t)frame->payload[offset + 3];
        offset += 4;
        if (offset + payload_len > frame->payload_len) {
            cmq_send_error(c, "invalid batch");
            return;
        }
        if (srv->config.max_payload_size > 0 &&
            payload_len > (uint32_t)srv->config.max_payload_size) {
            cmq_atomic_fetch_add_u64(&srv->stat_publishes_rejected, 1,
                                      CMQ_ATOMIC_RELAXED);
            cmq_send_error(c, "payload too large");
            return;
        }
        offset += payload_len;
    }
    if (offset != frame->payload_len) {
        cmq_send_error(c, "invalid batch");
        return;
    }

    /* Pass 2a: snapshot all deliveries before any fan-out (atomic batch). */
    typedef struct {
        char subject[CMQ_MAX_SUBJECT];
        uint16_t subject_len;
        uint16_t reply_len;
        size_t entry_start;
        const uint8_t *msg_payload;
        uint32_t payload_len;
        cmq_deliver_tgt_t *tgts;
        size_t ntgt;
        int cluster_ok; /* 1 = remote-only ingress OK / no cluster miss */
    } batch_prep_t;

    batch_prep_t *prep = calloc((size_t)count, sizeof(batch_prep_t));
    if (!prep) {
        cmq_send_error(c, "delivery failed");
        return;
    }

    offset = 2;
    for (uint16_t msg = 0; msg < count; msg++) {
        uint16_t subject_len = ((uint16_t)frame->payload[offset] << 8) |
                                frame->payload[offset + 1];
        offset += 2;
        memcpy(prep[msg].subject, frame->payload + offset, subject_len);
        prep[msg].subject[subject_len] = '\0';
        prep[msg].subject_len = subject_len;
        offset += subject_len;

        uint16_t reply_len = ((uint16_t)frame->payload[offset] << 8) |
                              frame->payload[offset + 1];
        prep[msg].entry_start = offset - 2 - subject_len;
        prep[msg].reply_len = reply_len;
        offset += 2 + (size_t)reply_len;

        uint32_t payload_len = ((uint32_t)frame->payload[offset] << 24) |
                                ((uint32_t)frame->payload[offset + 1] << 16) |
                                ((uint32_t)frame->payload[offset + 2] << 8) |
                                (uint32_t)frame->payload[offset + 3];
        offset += 4;
        prep[msg].msg_payload = frame->payload + offset;
        prep[msg].payload_len = payload_len;
        offset += payload_len;

        cmq_rwlock_rdlock(&srv->sublist_lock);
        cmq_sublist_result_t result;
        if (cmq_sublist_match(srv->sublist, prep[msg].subject, &result) != 0) {
            cmq_rwlock_unlock(&srv->sublist_lock);
            for (uint16_t k = 0; k <= msg; k++) free(prep[k].tgts);
            free(prep);
            cmq_send_error(c, "delivery failed");
            return;
        }
        size_t ntgt = 0;
        cmq_deliver_tgt_t *tgts = snapshot_deliver_targets(srv, &result, &ntgt);
        cmq_sublist_result_free(&result);
        cmq_rwlock_unlock(&srv->sublist_lock);
        if (ntgt == SIZE_MAX) {
            for (uint16_t k = 0; k < msg; k++) free(prep[k].tgts);
            free(prep);
            cmq_send_error(c, "delivery failed");
            return;
        }
        prep[msg].tgts = tgts;
        prep[msg].ntgt = ntgt;
    }

    /* Pass 2b-pre: refuse entire batch only when NO entry has local targets and
       cluster peers are configured but none live. Mixed batches (some local
       subs) must reach pass 2c — align with single-PUBLISH local delivery. */
    if (srv->routes && !c->is_route && cmq_route_pool_count(srv->routes) > 0 &&
        cmq_route_live_count(srv->routes) == 0 &&
        cmq_route_held_count(srv->routes) == 0) {
        int any_local = 0;
        for (uint16_t msg = 0; msg < count; msg++) {
            if (prep[msg].tgts && prep[msg].ntgt > 0) {
                any_local = 1;
                break;
            }
        }
        if (!any_local) {
            for (uint16_t k = 0; k < count; k++) free(prep[k].tgts);
            free(prep);
            cmq_send_error(c, "route failed");
            return;
        }
    }

    /* Pass 2b: cluster forward first (same order as single PUBLISH) so a
       remote-only failure cannot ERROR after local subscribers already got
       the batch. Attempt every entry — do not skip the tail after a miss. */
    int batch_fail = 0;
    int any_delivered = 0;
    for (uint16_t msg = 0; msg < count; msg++) {
        uint16_t subject_len = prep[msg].subject_len;
        uint16_t reply_len = prep[msg].reply_len;
        uint32_t payload_len = prep[msg].payload_len;
        const uint8_t *msg_payload = prep[msg].msg_payload;
        size_t route_sent = 0;

        if (srv->routes && !c->is_route) {
            if ((size_t)payload_len > SIZE_MAX - (4u + (size_t)subject_len +
                                                   (size_t)reply_len)) {
                batch_fail = 1;
                continue;
            }
            size_t pub_len = 2 + subject_len + 2 + reply_len + payload_len;
            uint8_t *pub = malloc(pub_len);
            if (pub) {
                size_t po = 0;
                pub[po++] = (subject_len >> 8) & 0xFF;
                pub[po++] = subject_len & 0xFF;
                memcpy(pub + po, prep[msg].subject, subject_len); po += subject_len;
                pub[po++] = (reply_len >> 8) & 0xFF;
                pub[po++] = reply_len & 0xFF;
                if (reply_len > 0) {
                    memcpy(pub + po,
                           frame->payload + prep[msg].entry_start + 2 +
                               subject_len + 2,
                           reply_len);
                    po += reply_len;
                }
                if (payload_len > 0)
                    memcpy(pub + po, msg_payload, payload_len);
                int route_rc = cmq_route_forward_op(srv, CMQ_OP_PUBLISH, 0,
                                                     pub, pub_len, &route_sent);
                free(pub);
                if (route_sent > 0)
                    any_delivered = 1;
                /* Remote-only entries must reach at least one live peer. */
                if (prep[msg].ntgt == 0 &&
                    cmq_route_forward_missed(srv, route_rc, route_sent))
                    batch_fail = 1;
                else
                    prep[msg].cluster_ok = 1;
            } else {
                /* Encode OOM for this entry — keep going so pass 2c can still
                   deliver locals (align with remote-only miss / no early abort). */
                cmq_atomic_fetch_add_u64(&srv->stat_messages_dropped, 1,
                                          CMQ_ATOMIC_RELAXED);
                batch_fail = 1;
            }
        } else {
            prep[msg].cluster_ok = 1;
        }
    }

    /* Pass 2c: always attempt local delivers — even after a remote-only cluster
       failure — so subscribers are not starved when earlier entries already
       reached the cluster (align with single-PUBLISH local-after-route). */
    for (uint16_t msg = 0; msg < count; msg++) {
        const uint8_t *msg_payload = prep[msg].msg_payload;
        uint32_t payload_len = prep[msg].payload_len;
        if (prep[msg].tgts && prep[msg].ntgt > 0) {
            if (deliver_targets_sync(srv, prep[msg].tgts, prep[msg].ntgt,
                                      prep[msg].subject, c->account_name,
                                      msg_payload, payload_len,
                                      NULL, 0) != 0)
                batch_fail = 1;
            else {
                any_delivered = 1;
                cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1,
                                          CMQ_ATOMIC_RELAXED);
                cmq_account_t *acc =
                    cmq_account_get(srv->accounts, c->account_name, NULL);
                if (acc) {
                    cmq_account_inc_msgs_in(acc, c->account_epoch,
                                             (uint64_t)payload_len);
                    cmq_account_release(srv->accounts, acc);
                }
            }
        } else if (prep[msg].cluster_ok) {
            /* Remote-only success path: count ingress when this entry's
               cluster pass OK (independent of earlier misses). */
            cmq_atomic_fetch_add_u64(&srv->stat_messages_in, 1, CMQ_ATOMIC_RELAXED);
            cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, NULL);
            if (acc) {
                cmq_account_inc_msgs_in(acc, c->account_epoch,
                                         (uint64_t)payload_len);
                cmq_account_release(srv->accounts, acc);
            }
        }
        free(prep[msg].tgts);
        prep[msg].tgts = NULL;
    }
    free(prep);
    /* Avoid ERROR after partial local/remote success (retry would duplicate). */
    if (batch_fail && !any_delivered)
        cmq_send_error(c, "batch delivery failed");
}

static void handle_frame(cmq_server_t *srv, cmq_client_t *c,
                          const cmq_frame_t *frame) {
    /* Only CONNECT/DISCONNECT before authentication — PING alone must not
       refresh INIT keepalive (slot-exhaustion DoS). */
    if (c->state != CMQ_CLIENT_CONNECTED &&
        frame->hdr.op != CMQ_OP_CONNECT &&
        frame->hdr.op != CMQ_OP_DISCONNECT) {
        cmq_send_error(c, "not connected");
        return;
    }

    cmq_atomic_fetch_add_u64(&srv->stat_bytes_in,
                              (uint64_t)(sizeof(cmq_frame_hdr_t) + frame->payload_len),
                              CMQ_ATOMIC_RELAXED);

    switch (frame->hdr.op) {
    case CMQ_OP_CONNECT:
        /* Only virgin INIT sockets may CONNECT — CLOSING must not resurrect. */
        if (c->state == CMQ_CLIENT_CLOSING || c->state == CMQ_CLIENT_CLOSED) {
            cmq_send_connack(c, 1);
            break;
        }
        if (c->state == CMQ_CLIENT_CONNECTED) {
            cmq_send_connack(c, 1);
            break;
        }
        if (auth_configured(srv)) {
            char uname[256] = {0};
            char passwd[256] = {0};
            char expect_u[256] = {0};
            char expect_p[256] = {0};
            int malformed = 0;
            if (!frame->payload || frame->payload_len < 4) {
                malformed = 1;
            } else {
                uint16_t ulen = ((uint16_t)frame->payload[0] << 8) |
                                 frame->payload[1];
                uint16_t plen = ((uint16_t)frame->payload[2] << 8) |
                                 frame->payload[3];
                if ((size_t)(4 + ulen + plen) > frame->payload_len ||
                    ulen >= 256 || plen >= 256) {
                    malformed = 1;
                } else {
                    if (ulen > 0) memcpy(uname, frame->payload + 4, ulen);
                    if (plen > 0) memcpy(passwd, frame->payload + 4 + ulen, plen);
                }
            }
            if (srv->config.auth_username && srv->config.auth_username[0])
                strncpy(expect_u, srv->config.auth_username, sizeof(expect_u) - 1);
            if (srv->config.auth_password && srv->config.auth_password[0])
                strncpy(expect_p, srv->config.auth_password, sizeof(expect_p) - 1);
            /* Always run padded compares so early rejects share timing with auth. */
            int need_user = (srv->config.auth_username &&
                             srv->config.auth_username[0]);
            int need_pass = (srv->config.auth_password &&
                             srv->config.auth_password[0]);
            int bad = malformed;
            if (need_user)
                bad |= !ct_memeq(uname, expect_u, sizeof(uname));
            else
                bad |= !ct_memeq(uname, uname, sizeof(uname)); /* timing pad */
            if (need_pass)
                bad |= !ct_memeq(passwd, expect_p, sizeof(passwd));
            else
                bad |= !ct_memeq(passwd, passwd, sizeof(passwd));
            if (bad) {
                cmq_send_connack(c, malformed ? 1 : 2);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            free(c->username);
            /* Password-only auth: ignore client username so a shared secret
               cannot pick/create arbitrary tenant accounts. */
            if (need_user)
                c->username = strdup(uname);
            else
                c->username = strdup("");
            if (!c->username) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
        }
        /* CMQ_FLAG_ROUTE only trusted for peers whose IP is in routes[].
           Inbound source ports are ephemeral — IP ACL + optional shared auth.
           Defer pool attach until after account bind so broadcast cannot hit
           an INIT peer (connected=1 before CONNACK). */
        int want_route = 0;
        int route_ri = -1;
        if (frame->hdr.flags & CMQ_FLAG_ROUTE) {
            route_ri = peer_route_index(srv, c->fd);
            if (!srv->routes || route_ri < 0) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            want_route = 1;
        }
        /* Resolve account before CONNECTED so OOM/table-full never lands on
           $default after a successful password check. */
        if (c->username && c->username[0] != '\0') {
            size_t ul = strnlen(c->username, CMQ_ACCOUNT_NAME_SIZE);
            if (ul == 0 || ul >= CMQ_ACCOUNT_NAME_SIZE) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            memcpy(c->account_name, c->username, ul);
            c->account_name[ul] = '\0';
            if (cmq_account_ensure(srv->accounts, c->account_name) != 0) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
        } else {
            strncpy(c->account_name, "$default", CMQ_ACCOUNT_NAME_SIZE - 1);
            c->account_name[CMQ_ACCOUNT_NAME_SIZE - 1] = '\0';
            /* Soft-delete must deny anonymous CONNECT until admin create(). */
            if (cmq_account_ensure(srv->accounts, "$default") != 0) {
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
        }
        /* ensure→get TOCTOU: soft-delete between them must not CONNACK 0. */
        uint32_t aep = 0;
        cmq_account_t *acc = cmq_account_get(srv->accounts, c->account_name, &aep);
        if (!acc ||
            !__atomic_load_n(&acc->active, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&acc->epoch, __ATOMIC_ACQUIRE) != aep) {
            if (acc) cmq_account_release(srv->accounts, acc);
            cmq_send_connack(c, 1);
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        c->account_epoch = aep;
        /* get→inc→CONNECTED TOCTOU: refuse if credit cannot stick or epoch died. */
        if (cmq_account_inc_connections(acc, aep) != 0) {
            cmq_account_release(srv->accounts, acc);
            c->account_epoch = 0;
            cmq_send_connack(c, 1);
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        if (!client_account_live(srv, c)) {
            cmq_account_dec_connections(acc, aep);
            cmq_account_release(srv->accounts, acc);
            c->account_epoch = 0;
            cmq_send_connack(c, 1);
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        cmq_account_release(srv->accounts, acc);
        /* Publish CONNECTED under clients_lock so keepalive cannot see
           CONNECTED with torn account_name/epoch (written while INIT). */
        {
            cmq_mutex_t *clk = client_clients_lock(srv, c);
            cmq_mutex_lock(clk);
            c->state = CMQ_CLIENT_CONNECTED;
            client_touch_activity(c);
            cmq_mutex_unlock(clk);
        }
        cmq_atomic_fetch_add_u64(&srv->stat_connections, 1, CMQ_ATOMIC_RELAXED);
        c->session_accounted = 1;
        /* Stage inbound (connected=0) before CONNACK so failure can still
           send CONNACK 1; promote after drain so broadcast cannot race. */
        if (want_route) {
            char nid[CMQ_NODE_ID_SIZE];
            snprintf(nid, sizeof(nid), "r%d", route_ri);
            if (cmq_route_attach_inbound(srv->routes, nid, c->fd) != 0) {
                /* CONNACK before CLOSING — send_direct rejects CLOSING. */
                cmq_send_connack(c, 1);
                c->state = CMQ_CLIENT_CLOSING;
                break;
            }
            c->is_route = 1;
        }
        if (!c->is_websocket)
            send_info_frame(srv, c);
        cmq_send_connack(c, 0);
        if (want_route) {
            if (client_drain_write_sync(c) != 0 ||
                c->state != CMQ_CLIENT_CONNECTED) {
                /* Staged fd still looks live to peer_live — detach now so
                   outbound reconnect is not blocked until teardown. */
                cmq_route_detach_fd(srv->routes, c->fd);
                c->is_route = 0;
                c->state = CMQ_CLIENT_CLOSING;
                (void)shutdown(c->fd, SHUT_RDWR);
                break;
            }
            if (cmq_route_mark_connected(srv->routes, c->fd) != 0) {
                cmq_route_detach_fd(srv->routes, c->fd);
                c->is_route = 0;
                c->state = CMQ_CLIENT_CLOSING;
                (void)shutdown(c->fd, SHUT_RDWR);
                break;
            }
        }
        break;
    case CMQ_OP_PING:
        if (!client_account_live(srv, c)) {
            cmq_send_error(c, "account inactive");
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        cmq_send_pong(c);
        break;
    case CMQ_OP_PUBLISH:
    case CMQ_OP_REQUEST:
    case CMQ_OP_RESPONSE:
    case CMQ_OP_SUBSCRIBE:
    case CMQ_OP_UNSUBSCRIBE:
    case CMQ_OP_BATCH:
        if (!client_account_live(srv, c)) {
            cmq_send_error(c, "account inactive");
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        if (frame->hdr.op == CMQ_OP_PUBLISH)
            handle_publish(srv, c, frame);
        else if (frame->hdr.op == CMQ_OP_REQUEST)
            handle_request(srv, c, frame);
        else if (frame->hdr.op == CMQ_OP_RESPONSE)
            handle_response(srv, c, frame);
        else if (frame->hdr.op == CMQ_OP_SUBSCRIBE)
            handle_subscribe(srv, c, frame);
        else if (frame->hdr.op == CMQ_OP_UNSUBSCRIBE)
            handle_unsubscribe(srv, c, frame);
        else
            handle_batch(srv, c, frame);
        break;
    case CMQ_OP_DISCONNECT:
        c->state = CMQ_CLIENT_CLOSING;
        break;
    case CMQ_OP_STATS:
        if (!client_account_live(srv, c)) {
            cmq_send_error(c, "account inactive");
            c->state = CMQ_CLIENT_CLOSING;
            break;
        }
        handle_stats(srv, c);
        break;
    default:
        cmq_send_error(c, "unknown op");
        break;
    }
}

/* Caller holds srv->clients_lock for acceptor-owned clients (worker_id < 0). */
static void client_flush_write_unlocked(cmq_client_t *c) {
    int route_io_idx = -1;
    if (c->is_route && c->server && c->server->routes && c->fd >= 0) {
        route_io_idx = cmq_route_io_lock_fd(c->server->routes, c->fd);
        if (route_io_idx < 0) {
            c->write_len = 0;
            c->write_pos = 0;
            client_clear_write_progress(c);
            c->state = CMQ_CLIENT_CLOSING;
            if (c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            return;
        }
    }

    if (!c->write_buf || c->write_pos >= c->write_len) {
        c->write_len = 0;
        c->write_pos = 0;
        client_clear_write_progress(c);
        /* Keep write_buf/write_cap for reuse on the next send. */
        if (c->fd >= 0 &&
            cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c) != 0) {
            /* Still armed WRITE → level-trigger busy loop; force EOF. */
            if (route_io_idx >= 0 && c->server && c->server->routes) {
                cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
                route_io_idx = -1;
            }
            (void)shutdown(c->fd, SHUT_RDWR);
            return;
        }
        goto out;
    }

    size_t remaining = c->write_len - c->write_pos;
    ssize_t n = client_sock_write(c, c->write_buf + c->write_pos, remaining);
    if (n > 0) {
        c->write_pos += (size_t)n;
        client_mark_write_progress(c);
        cmq_atomic_fetch_add_u64(&c->server->stat_bytes_out, (uint64_t)n,
                                  CMQ_ATOMIC_RELAXED);
        if (c->write_pos >= c->write_len) {
            c->write_len = 0;
            c->write_pos = 0;
            client_clear_write_progress(c);
            if (cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c) != 0) {
                if (route_io_idx >= 0 && c->server && c->server->routes) {
                    cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
                    route_io_idx = -1;
                }
                (void)shutdown(c->fd, SHUT_RDWR);
                return;
            }
        }
    } else if (n <= 0 &&
               (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))) {
        /* Hard write failure — clear stuck write_buf; do NOT teardown here
           (caller still holds c). Force CLOSING + SHUT_RDWR for owning path. */
        c->write_len = 0;
        c->write_pos = 0;
        client_clear_write_progress(c);
        if (route_io_idx >= 0 && c->server && c->server->routes) {
            cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
            route_io_idx = -1;
        }
        client_force_closing(c);
        if (c->fd >= 0)
            (void)shutdown(c->fd, SHUT_RDWR);
        return;
    }
out:
    if (route_io_idx >= 0 && c->server && c->server->routes)
        cmq_route_io_unlock_idx(c->server->routes, route_io_idx);
}

/* Serialize acceptor write_buf with worker send_local (holds clients_lock). */
static void client_flush_write(cmq_client_t *c) {
    cmq_server_t *srv = c ? c->server : NULL;
    int acc = (c && c->worker_id < 0 && srv != NULL);
    if (acc)
        cmq_mutex_lock(&srv->clients_lock);
    client_flush_write_unlocked(c);
    if (acc)
        cmq_mutex_unlock(&srv->clients_lock);
}

static void send_info_frame(cmq_server_t *srv, cmq_client_t *c) {
    uint8_t info_buf[256];
    uint64_t conns = cmq_atomic_load_u64(&srv->stat_connections, CMQ_ATOMIC_RELAXED);
    uint64_t subs = cmq_atomic_load_u64(&srv->stat_subscriptions, CMQ_ATOMIC_RELAXED);
    char info_json[256];
    int info_len = snprintf(info_json, sizeof(info_json),
        "{\"version\":\"0.1.0\",\"proto\":1,\"connections\":%llu,\"subscriptions\":%llu,\"auth\":%s}",
        (unsigned long long)conns, (unsigned long long)subs,
        auth_configured(srv) ? "true" : "false");
    /* Truncated snprintf returns would-be length — never encode past buffer. */
    if (info_len > 0 && (size_t)info_len < sizeof(info_json)) {
        size_t len = cmq_frame_encode(info_buf, sizeof(info_buf), CMQ_OP_INFO, 0,
                                       (const uint8_t *)info_json, (size_t)info_len);
        if (len > 0) cmq_client_send(c, info_buf, len);
    }
    c->info_sent = 1;
}

static int http_header_value(const char *req, const char *name,
                              char *out, size_t out_sz) {
    if (!req || !name || !out || out_sz == 0) return -1;
    size_t nlen = strlen(name);
    const char *p = req;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t llen = (size_t)(p - line);
        if (llen > nlen + 1) {
            int match = 1;
            for (size_t i = 0; i < nlen; i++) {
                char a = line[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (match && line[nlen] == ':') {
                const char *v = line + nlen + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vlen = (size_t)((line + llen) - v);
                /* Fail closed — truncated Origin/Host must not pass CSWSH. */
                if (vlen >= out_sz) return -1;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return 0;
            }
        }
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    return -1;
}

/* RFC 7230 Connection tokens are comma-separated and case-insensitive. */
static int http_header_has_token(const char *val, const char *tok) {
    if (!val || !tok || !tok[0]) return 0;
    size_t tlen = strlen(tok);
    const char *p = val;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (len == tlen) {
            int match = 1;
            for (size_t i = 0; i < tlen; i++) {
                char a = start[i], b = tok[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (match) return 1;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }
    return 0;
}

/* Extract host[:port] from Origin URL or Host header into out.
   Returns 0 on success, -1 if the host would be truncated. */
static int http_host_from_value(const char *val, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!val || out_sz == 0) return -1;
    const char *host = val;
    if (strncmp(val, "http://", 7) == 0) host = val + 7;
    else if (strncmp(val, "https://", 8) == 0) host = val + 8;
    else if (strncmp(val, "ws://", 5) == 0) host = val + 5;
    else if (strncmp(val, "wss://", 6) == 0) host = val + 6;
    size_t i = 0;
    while (host[i] && host[i] != '/' && host[i] != '?') {
        if (i + 1 >= out_sz) {
            out[0] = '\0';
            return -1;
        }
        char ch = host[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        out[i++] = ch;
    }
    out[i] = '\0';
    /* Normalize host:port — Origin often omits default/explicit port vs Host. */
    if (out[0] == '[') {
        char *rb = strchr(out, ']');
        if (rb && rb[1] == ':') rb[1] = '\0';
    } else {
        char *colon = strrchr(out, ':');
        if (colon && colon[1]) {
            int digits = 1;
            for (const char *p = colon + 1; *p; p++) {
                if (*p < '0' || *p > '9') { digits = 0; break; }
            }
            if (digits) *colon = '\0';
        }
    }
    return out[0] ? 0 : -1;
}

static int handle_ws_upgrade(cmq_client_t *c, const uint8_t *data, size_t len,
                              size_t *consumed) {
    if (consumed) *consumed = 0;
    if (len < 4) return 1; /* need more data */

    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end == 0) return 1; /* incomplete HTTP headers */

    /* Full header buffer (up to ws_recv_cap) — do not truncate Key/Origin. */
    char *req = malloc(hdr_end + 1);
    if (!req) return -1;
    memcpy(req, data, hdr_end);
    req[hdr_end] = '\0';

    if (strncmp(req, "GET ", 4) != 0) { free(req); return -1; }

    /* Header names/values: RFC 7230 case-insensitive (align with WS Key parse). */
    char upgrade[64] = {0}, version[32] = {0};
    if (http_header_value(req, "Upgrade", upgrade, sizeof(upgrade)) != 0) {
        free(req);
        return -1;
    }
    for (char *p = upgrade; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    if (strcmp(upgrade, "websocket") != 0) { free(req); return -1; }
    if (http_header_value(req, "Sec-WebSocket-Version", version,
                           sizeof(version)) != 0) {
        free(req);
        return -1;
    }
    /* Trim incidental spaces already handled by http_header_value. */
    if (strcmp(version, "13") != 0) { free(req); return -1; }

    /* RFC 6455 §4.2.1/§4.2.2 — Connection must list upgrade (token form). */
    char connection[256] = {0};
    if (http_header_value(req, "Connection", connection, sizeof(connection)) != 0 ||
        !http_header_has_token(connection, "upgrade")) {
        free(req);
        return -1;
    }

    /* CSWSH: if Origin is present it must match Host. Native clients may omit Origin. */
    char origin_raw[256] = {0}, host_raw[256] = {0};
    char origin_host[256] = {0}, host_host[256] = {0};
    if (http_header_value(req, "Origin", origin_raw, sizeof(origin_raw)) == 0) {
        if (http_header_value(req, "Host", host_raw, sizeof(host_raw)) != 0) {
            free(req);
            return -1;
        }
        if (http_host_from_value(origin_raw, origin_host, sizeof(origin_host)) != 0 ||
            http_host_from_value(host_raw, host_host, sizeof(host_host)) != 0 ||
            strcmp(origin_host, host_host) != 0) {
            free(req);
            return -1;
        }
    }

    char ws_key[128] = {0};
    if (cmq_ws_parse_http_upgrade(req, hdr_end, ws_key, sizeof(ws_key)) != 0) {
        free(req);
        return -1;
    }
    free(req);

    char accept_key[64] = {0};
    if (cmq_ws_accept_key(ws_key, accept_key, sizeof(accept_key)) != 0)
        return -1;

    char response[512];
    if (cmq_ws_build_response(accept_key, response, sizeof(response)) != 0)
        return -1;

    size_t resp_len = strlen(response);
    free(c->write_buf);
    c->write_buf = NULL;
    c->write_cap = 0;
    c->write_len = 0;
    c->write_pos = 0;
    client_clear_write_progress(c);
    if (cmq_client_send(c, (const uint8_t *)response, resp_len) != 0)
        return -1;

    c->is_websocket = 1;
    c->ws_upgrade_done = 1;
    if (consumed) *consumed = hdr_end;
    return 0;
}

static void client_closing_discard_inbound(cmq_client_t *c) {
    if (!c || c->fd < 0) return;
    uint8_t junk[2048];
    for (;;) {
        ssize_t n = client_sock_read(c, junk, sizeof(junk));
        if (n > 0) continue;
        break; /* EAGAIN/EWOULDBLOCK, EOF, or hard error */
    }
}

static void client_finish_closing(cmq_client_t *c) {
    if (!c || c->state != CMQ_CLIENT_CLOSING) return;
    /* Acceptor-owned: serialize with worker REQUEST drain (holds clients_lock). */
    cmq_server_t *srv = c->server;
    int acc = (c->worker_id < 0 && srv != NULL);
    if (acc)
        cmq_mutex_lock(&srv->clients_lock);
    if (c->state != CMQ_CLIENT_CLOSING) {
        if (acc)
            cmq_mutex_unlock(&srv->clients_lock);
        return;
    }
    if (c->write_buf && c->write_pos < c->write_len) {
        /* Peer may still send; drain RX so our final frames are not blocked
           by a full TCP receive window (keepalive skips CLOSING). */
        client_closing_discard_inbound(c);
        /* Already hold clients_lock for acceptor — avoid recursive lock. */
        client_flush_write_unlocked(c);
        /* flush_write never destroys c; hard error leaves CLOSING + empty buf. */
        if (c->write_buf && c->write_pos < c->write_len) {
            /* Keep READ+WRITE so inbound continues to be discarded. */
            if (c->fd >= 0 &&
                cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE,
                           client_read_cb, c) != 0) {
                (void)shutdown(c->fd, SHUT_RDWR);
            }
            if (acc)
                cmq_mutex_unlock(&srv->clients_lock);
            return;
        }
    }
    if (acc)
        cmq_mutex_unlock(&srv->clients_lock);
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
        if (events & CMQ_EV_READ)
            client_closing_discard_inbound(c);
        client_finish_closing(c);
        return;
    }

    if (!(events & CMQ_EV_READ)) return;

    ssize_t n = client_sock_read(c, c->read_buf, sizeof(c->read_buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            client_teardown(c);
        }
        return;
    }
    /* INIT: do not refresh on TCP traffic — PING/junk must not hold slots.
       WS: only complete frames / control refresh (see below) — FIN=0 spam
       must not bypass idle keepalive. */
    if (c->state == CMQ_CLIENT_CONNECTED && !c->is_websocket)
        client_touch_activity(c);

    /* HTTP upgrade may arrive fragmented or pipelined with the first WS frame.
       Accumulate into ws_recv_buf until headers complete, then keep any trailing
       bytes for the WS frame parser below. */
    if (!c->is_websocket && !c->ws_upgrade_done &&
        (c->ws_recv_len > 0 || (n > 0 && c->read_buf[0] == 'G'))) {
        if ((size_t)n > SIZE_MAX - c->ws_recv_len) { client_teardown(c); return; }
        size_t need = c->ws_recv_len + (size_t)n;
        if (need > c->ws_recv_cap) {
            size_t ncap = c->ws_recv_cap ? c->ws_recv_cap : 4096;
            if (c->ws_recv_cap) {
                if (c->ws_recv_cap > SIZE_MAX / 2) ncap = 65536;
                else ncap = c->ws_recv_cap * 2;
            }
            while (ncap < need) {
                if (ncap > SIZE_MAX / 2) { ncap = 65536; break; }
                ncap *= 2;
            }
            if (ncap > 65536) ncap = 65536;
            if (need > ncap) { client_teardown(c); return; }
            uint8_t *nb = realloc(c->ws_recv_buf, ncap);
            if (!nb) { client_teardown(c); return; }
            c->ws_recv_buf = nb;
            c->ws_recv_cap = ncap;
        }
        memcpy(c->ws_recv_buf + c->ws_recv_len, c->read_buf, (size_t)n);
        c->ws_recv_len += (size_t)n;
        n = 0; /* consumed into ws_recv_buf */

        size_t consumed = 0;
        int urc = handle_ws_upgrade(c, c->ws_recv_buf, c->ws_recv_len, &consumed);
        if (urc == 1) return; /* incomplete HTTP — wait for more */
        if (urc != 0) {
            /* Not a valid WS upgrade; fall through to CMQ if buffer looks binary,
               otherwise tear down. */
            if (c->ws_recv_buf[0] == 'G') {
                client_teardown(c);
                return;
            }
        } else {
            /* Drop HTTP headers; keep any pipelined WS bytes in ws_recv_buf. */
            if (consumed < c->ws_recv_len) {
                size_t rest = c->ws_recv_len - consumed;
                memmove(c->ws_recv_buf, c->ws_recv_buf + consumed, rest);
                c->ws_recv_len = rest;
            } else {
                c->ws_recv_len = 0;
            }
        }
    }

    if (c->is_websocket && c->ws_upgrade_done) {
        /* Append any fresh TCP bytes into reassembly buffer. */
        if (n > 0) {
            if ((size_t)n > SIZE_MAX - c->ws_recv_len) { client_teardown(c); return; }
            size_t need = c->ws_recv_len + (size_t)n;
            if (need > c->ws_recv_cap) {
                size_t hard = cmq_client_frame_hard_cap(srv);
                size_t ncap = c->ws_recv_cap ? c->ws_recv_cap : 4096;
                if (c->ws_recv_cap) {
                    if (c->ws_recv_cap > SIZE_MAX / 2) ncap = hard;
                    else ncap = c->ws_recv_cap * 2;
                }
                while (ncap < need) {
                    if (ncap > SIZE_MAX / 2) { ncap = hard; break; }
                    ncap *= 2;
                }
                if (ncap > hard) ncap = hard;
                if (need > ncap) { client_teardown(c); return; }
                uint8_t *nb = realloc(c->ws_recv_buf, ncap);
                if (!nb) { client_teardown(c); return; }
                c->ws_recv_buf = nb;
                c->ws_recv_cap = ncap;
            }
            memcpy(c->ws_recv_buf + c->ws_recv_len, c->read_buf, (size_t)n);
            c->ws_recv_len += (size_t)n;
        }

        size_t offset = 0;
        while (offset < c->ws_recv_len) {
            cmq_ws_frame_t ws_frame;
            int parsed = cmq_ws_frame_parse(c->ws_recv_buf + offset,
                                             c->ws_recv_len - offset, &ws_frame);
            if (parsed < 0) {
                client_teardown(c); /* fatal WS framing */
                return;
            }
            if (parsed == 0) break; /* need more data */

            /* RFC 6455: client→server frames MUST be masked. */
            if (!ws_frame.masked) {
                client_teardown(c);
                return;
            }

            if (ws_frame.opcode == CMQ_WS_OPCODE_CLOSE) {
                client_teardown(c);
                return;
            }

            if (ws_frame.opcode == CMQ_WS_OPCODE_PING) {
                /* RFC 6455: reply with PONG echoing the application data. */
                if (ws_frame.payload_len > 125) {
                    client_teardown(c);
                    return;
                }
                /* Soft-deleted / epoch-dead accounts must not refresh keepalive
                   via WS ping (align with CMQ_OP_PING). */
                if (c->state == CMQ_CLIENT_CONNECTED &&
                    !client_account_live(srv, c)) {
                    c->state = CMQ_CLIENT_CLOSING;
                    client_finish_closing(c);
                    return;
                }
                uint8_t pong[140];
                uint8_t unmasked[125];
                const uint8_t *app = ws_frame.payload;
                size_t alen = ws_frame.payload_len;
                if (alen > 0 && ws_frame.masked) {
                    memcpy(unmasked, ws_frame.payload, alen);
                    cmq_ws_mask(unmasked, alen, ws_frame.mask_key);
                    app = unmasked;
                }
                cmq_ws_frame_t pf;
                pf.fin = 1;
                pf.opcode = CMQ_WS_OPCODE_PONG;
                pf.payload = app;
                pf.payload_len = alen;
                pf.mask_key = 0;
                pf.masked = 0;
                int plen = cmq_ws_frame_serialize(&pf, pong, sizeof(pong));
                if (plen > 0)
                    cmq_client_send_direct(c, pong, (size_t)plen);
                /* Only CONNECTED may refresh keepalive — WS ping must not
                   hold INIT slots forever (same rule as CMQ PING). */
                if (c->state == CMQ_CLIENT_CONNECTED)
                    client_touch_activity(c);
                offset += (size_t)parsed;
                continue;
            }
            if (ws_frame.opcode == CMQ_WS_OPCODE_PONG) {
                if (c->state == CMQ_CLIENT_CONNECTED &&
                    !client_account_live(srv, c)) {
                    c->state = CMQ_CLIENT_CLOSING;
                    client_finish_closing(c);
                    return;
                }
                if (c->state == CMQ_CLIENT_CONNECTED)
                    client_touch_activity(c);
                offset += (size_t)parsed;
                continue;
            }

            int is_data = (ws_frame.opcode == CMQ_WS_OPCODE_BINARY ||
                           ws_frame.opcode == CMQ_WS_OPCODE_TEXT ||
                           ws_frame.opcode == CMQ_WS_OPCODE_CONTINUATION);
            if (!is_data) {
                /* Reserved / unknown opcode — RFC 6455: close the connection. */
                client_teardown(c);
                return;
            }
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

                if (++c->ws_frag_count > 256) {
                    client_teardown(c);
                    return;
                }

                if (ws_frame.payload_len > 0) {
                    if (ws_frame.payload_len > SIZE_MAX - c->ws_msg_len) {
                        client_teardown(c);
                        return;
                    }
                    size_t need = c->ws_msg_len + ws_frame.payload_len;
                    if (need > c->ws_msg_cap) {
                        size_t hard = cmq_client_frame_hard_cap(srv);
                        size_t ncap = c->ws_msg_cap ? c->ws_msg_cap : 4096;
                        if (c->ws_msg_cap) {
                            if (c->ws_msg_cap > SIZE_MAX / 2) ncap = hard;
                            else ncap = c->ws_msg_cap * 2;
                        }
                        while (ncap < need) {
                            if (ncap > SIZE_MAX / 2) { ncap = hard; break; }
                            ncap *= 2;
                        }
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
                    if (c->state == CMQ_CLIENT_CONNECTED)
                        client_touch_activity(c);
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
                    c->ws_frag_count = 0;
                }
            }
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

static void keepalive_scan_clients(cmq_server_t *srv, cmq_client_t **clients,
                                    int count, uint64_t now,
                                    uint64_t timeout_ms,
                                    uint64_t write_timeout_ms) {
    /* Acceptor clients: take clients_lock so we serialize with worker drain.
       Under lock only force_closing/shutdown — teardown re-locks. */
    if (!srv || count <= 0) return;
    for (int i = 0; i < count; i++) {
        cmq_client_t *c = clients[i];
        if (!c) continue;
        cmq_mutex_lock(&srv->clients_lock);
        int present = 0;
        for (int j = 0; j < srv->clients_count; j++) {
            if (srv->clients[j] == c) {
                present = 1;
                break;
            }
        }
        if (!present || c->state == CMQ_CLIENT_CLOSED) {
            cmq_mutex_unlock(&srv->clients_lock);
            continue;
        }
        if (c->state == CMQ_CLIENT_CONNECTED &&
            !client_account_live(srv, c)) {
            client_force_closing(c);
            cmq_mutex_unlock(&srv->clients_lock);
            client_finish_closing(c);
            continue;
        }
        int stalled = client_write_stalled(c, now, write_timeout_ms);
        if (c->state == CMQ_CLIENT_CLOSING) {
            int closing_idle = (now - client_activity_ms(c)) > timeout_ms;
            if (stalled && c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            cmq_mutex_unlock(&srv->clients_lock);
            if (stalled || closing_idle)
                client_finish_closing(c);
            continue;
        }
        int idle = (c->state == CMQ_CLIENT_CONNECTED ||
                    c->state == CMQ_CLIENT_INIT) &&
                   (now - client_activity_ms(c)) > timeout_ms;
        if (!idle && !stalled) {
            cmq_mutex_unlock(&srv->clients_lock);
            continue;
        }
        if (c->state == CMQ_CLIENT_CONNECTED) {
            uint8_t disc[16];
            size_t disc_len = cmq_frame_encode(disc, sizeof(disc),
                                                CMQ_OP_DISCONNECT, 0, NULL, 0);
            if (disc_len > 0)
                (void)cmq_client_send_local(c, disc, disc_len);
            client_force_closing(c);
            cmq_mutex_unlock(&srv->clients_lock);
            client_finish_closing(c);
        } else {
            client_force_closing(c);
            if (c->fd >= 0)
                (void)shutdown(c->fd, SHUT_RDWR);
            cmq_mutex_unlock(&srv->clients_lock);
            client_finish_closing(c);
        }
    }
}

static void keepalive_timer_cb(int timer_id, int events, void *data) {
    (void)timer_id;
    (void)events;
    cmq_server_t *srv = (cmq_server_t *)data;
    int interval = srv->config.ping_interval_ms;
    if (interval <= 0) return;
    /* Validate caps interval; widen before *2 so no signed overflow. */
    uint64_t timeout_ms = (uint64_t)(unsigned)interval * 2u;
    uint64_t now = srv_now_ms();
    uint64_t write_timeout_ms = srv->config.write_timeout_ms > 0
                                    ? (uint64_t)srv->config.write_timeout_ms
                                    : 0;

    cmq_mutex_lock(&srv->clients_lock);
    int n = srv->clients_count;
    cmq_client_t **snap = NULL;
    if (n > 0) {
        snap = malloc((size_t)n * sizeof(cmq_client_t *));
        if (snap) {
            memcpy(snap, srv->clients, (size_t)n * sizeof(cmq_client_t *));
        } else {
            /* OOM: force-close idle/stalled/epoch-dead without heap snapshot. */
            for (int i = 0; i < n; i++) {
                cmq_client_t *c = srv->clients[i];
                if (!c || c->state == CMQ_CLIENT_CLOSED)
                    continue;
                if (c->state == CMQ_CLIENT_CONNECTED &&
                    !client_account_live(srv, c)) {
                    /* Safe under clients_lock: mark CLOSING, no teardown. */
                    client_force_closing(c);
                    continue;
                }
                int stalled = client_write_stalled(c, now, write_timeout_ms);
                if (c->state == CMQ_CLIENT_CLOSING) {
                    int closing_idle = (now - client_activity_ms(c)) > timeout_ms;
                    if ((stalled || closing_idle) && c->fd >= 0)
                        shutdown(c->fd, SHUT_RDWR);
                    continue;
                }
                int idle = (c->state == CMQ_CLIENT_CONNECTED ||
                            c->state == CMQ_CLIENT_INIT) &&
                           (now - client_activity_ms(c)) > timeout_ms;
                if (idle || stalled)
                    client_force_closing(c);
            }
        }
    }
    cmq_mutex_unlock(&srv->clients_lock);
    if (snap) {
        keepalive_scan_clients(srv, snap, n, now, timeout_ms, write_timeout_ms);
        free(snap);
    }

    if (srv->workers) {
        for (int wi = 0; wi < srv->num_workers; wi++) {
            cmq_worker_t *w = &srv->workers[wi];
            cmq_mutex_lock(&w->clients_lock);
            int wn = w->clients_count;
            enum { CMQ_KA_STACK = 128, CMQ_KA_OVERFLOW = 64 };
            typedef struct { uint32_t id; uint32_t gen; } cmq_doom_t;
            cmq_doom_t stack_doomed[CMQ_KA_STACK];
            cmq_doom_t stack_overflow[CMQ_KA_OVERFLOW];
            cmq_doom_t *doomed = NULL;
            int ndoomed = 0;
            int noverflow = 0;
            int doomed_heap = 0;
            if (wn > 0) {
                doomed = stack_doomed;
                int doomed_cap = CMQ_KA_STACK;
                if (wn > CMQ_KA_STACK) {
                    cmq_doom_t *heap = malloc((size_t)wn * sizeof(cmq_doom_t));
                    if (heap) {
                        doomed = heap;
                        doomed_cap = wn;
                        doomed_heap = 1;
                    }
                }
                for (int i = 0; i < wn; i++) {
                    cmq_client_t *c = w->clients[i];
                    if (!c || c->state == CMQ_CLIENT_CLOSED)
                        continue;
                    int stalled = client_write_stalled(c, now, write_timeout_ms);
                    int doom = 0;
                    if (c->state == CMQ_CLIENT_CONNECTED &&
                        !client_account_live(srv, c)) {
                        doom = 1; /* soft-deleted — reclaim without idle wait */
                    } else if (c->state == CMQ_CLIENT_CLOSING) {
                        int closing_idle = (now - client_activity_ms(c)) > timeout_ms;
                        doom = stalled || closing_idle;
                    } else {
                        int idle = (c->state == CMQ_CLIENT_CONNECTED ||
                                    c->state == CMQ_CLIENT_INIT) &&
                                   (now - client_activity_ms(c)) > timeout_ms;
                        doom = idle || stalled;
                    }
                    if (!doom)
                        continue;
                    if (ndoomed < doomed_cap) {
                        doomed[ndoomed].id = c->id;
                        doomed[ndoomed].gen = c->conn_gen;
                        ndoomed++;
                    } else if (noverflow < CMQ_KA_OVERFLOW) {
                        /* Heap snapshot failed — still TEARDOWN after unlock.
                           Do not cmq_ev_mod worker loops from the acceptor. */
                        stack_overflow[noverflow].id = c->id;
                        stack_overflow[noverflow].gen = c->conn_gen;
                        noverflow++;
                        if (c->fd >= 0)
                            shutdown(c->fd, SHUT_RDWR);
                    } else {
                        /* Beyond overflow slots: shutdown + post TEARDOWN now
                           (safe: worker never holds msg_lock across clients_lock). */
                        uint32_t tid = c->id, tgen = c->conn_gen;
                        if (c->fd >= 0)
                            shutdown(c->fd, SHUT_RDWR);
                        (void)worker_push_teardown(w, tid, tgen);
                    }
                }
            }
            cmq_mutex_unlock(&w->clients_lock);
            uint8_t disc[16];
            size_t disc_len = cmq_frame_encode(disc, sizeof(disc),
                                                CMQ_OP_DISCONNECT, 0, NULL, 0);
            for (int i = 0; i < ndoomed; i++) {
                if (disc_len > 0)
                    worker_push_msg(w, doomed[i].id, doomed[i].gen,
                                    disc, disc_len, 0, 0, NULL, 0, 0, NULL, 0,
                                    NULL, 0);
                if (worker_push_teardown(w, doomed[i].id, doomed[i].gen) != 0) {
                    /* OOM: force-close fd so the worker notices EOF. */
                    cmq_mutex_lock(&w->clients_lock);
                    for (int j = 0; j < w->clients_count; j++) {
                        cmq_client_t *c = w->clients[j];
                        if (c && c->id == doomed[i].id &&
                            c->conn_gen == doomed[i].gen && c->fd >= 0) {
                            shutdown(c->fd, SHUT_RDWR);
                            break;
                        }
                    }
                    cmq_mutex_unlock(&w->clients_lock);
                }
            }
            for (int i = 0; i < noverflow; i++) {
                if (worker_push_teardown(w, stack_overflow[i].id,
                                          stack_overflow[i].gen) != 0) {
                    cmq_mutex_lock(&w->clients_lock);
                    for (int j = 0; j < w->clients_count; j++) {
                        cmq_client_t *c = w->clients[j];
                        if (c && c->id == stack_overflow[i].id &&
                            c->conn_gen == stack_overflow[i].gen &&
                            c->fd >= 0) {
                            shutdown(c->fd, SHUT_RDWR);
                            break;
                        }
                    }
                    cmq_mutex_unlock(&w->clients_lock);
                }
            }
            if (doomed_heap)
                free(doomed);
        }
    }
}

static void *route_reconnect_thread(void *arg) {
    cmq_server_t *srv = arg;
    while (cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE)) {
        if (srv->routes && srv->config.route_count > 0) {
            for (int i = 0; i < srv->config.route_count; i++) {
                if (!cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE))
                    break;
                char nid[CMQ_NODE_ID_SIZE];
                snprintf(nid, sizeof(nid), "r%d", i);
                const char *addr = srv->config.routes[i].addr;
                int port = srv->config.routes[i].port;
                cmq_route_conn_t snap;
                if (cmq_route_get_conn(srv->routes, nid, &snap) == 0 &&
                    snap.fd >= 0 &&
                    (snap.remote_addr[0] == '\0' ||
                     (snap.remote_port == port &&
                      strncmp(snap.remote_addr, addr, sizeof(snap.remote_addr)) == 0)) &&
                    cmq_route_peer_live(srv->routes, nid))
                    continue;
                if (cmq_route_connect(srv->routes, nid, addr, port,
                                      srv->config.auth_username,
                                      srv->config.auth_password) == 0) {
                    cmq_log_info(srv->log, "Route reconnected to %s:%d",
                                 addr, port);
                }
                /* Continue — retry every dead peer each interval. */
            }
        }
        for (int s = 0; s < 10; s++) {
            if (!cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE))
                break;
            struct timespec ts = {0, 100000000L};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
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
    /* Drain/stop: never admit new sessions (covers backlog race after listen close). */
    if (!cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE) ||
        cmq_atomic_load_int(&srv->acceptor_drain, CMQ_ATOMIC_ACQUIRE))
        return;

    /* Drain the listen backlog — one accept per epoll wake stalls under bursts.
       Cap per wake so keepalive / existing clients stay responsive. */
    enum { CMQ_ACCEPT_PER_WAKE = 64 };
    int accepted = 0;
    for (;;) {
        if (accepted >= CMQ_ACCEPT_PER_WAKE)
            return;
        if (!cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE) ||
            cmq_atomic_load_int(&srv->acceptor_drain, CMQ_ATOMIC_ACQUIRE))
            return;
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int client_fd = accept(fd, (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            return; /* EAGAIN / EWOULDBLOCK / fatal */
        }
        accepted++;
        if (!cmq_atomic_load_int(&srv->running, CMQ_ATOMIC_ACQUIRE) ||
            cmq_atomic_load_int(&srv->acceptor_drain, CMQ_ATOMIC_ACQUIRE)) {
            close(client_fd);
            return;
        }

        if (set_nonblocking(client_fd) != 0) {
            close(client_fd);
            continue;
        }

        if (srv->config.max_clients > 0) {
            /* CAS so concurrent accepts cannot overshoot max_clients. */
            uint32_t max = (uint32_t)srv->config.max_clients;
            uint32_t cur = cmq_atomic_load_u32(&srv->active_clients,
                                                CMQ_ATOMIC_SEQ_CST);
            int admitted = 0;
            for (;;) {
                if (cur >= max)
                    break;
                if (cmq_atomic_cas_u32(&srv->active_clients, &cur, cur + 1,
                                        CMQ_ATOMIC_SEQ_CST)) {
                    admitted = 1;
                    break;
                }
            }
            if (!admitted) {
                close(client_fd);
                continue;
            }
        } else {
            cmq_atomic_fetch_add_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
        }

        uint32_t cid = 0;
        for (int id_try = 0; id_try < 16; id_try++) {
            uint32_t cand = cmq_atomic_fetch_add_u32(&srv->next_client_id, 1,
                                                      CMQ_ATOMIC_SEQ_CST);
            /* 0 = empty slot; UINT32_MAX = tombstone — never assign as client id. */
            if (cand != 0 && cand != CMQ_IDMAP_TOMB) {
                cid = cand;
                break;
            }
        }
        if (cid == 0) {
            close(client_fd);
            cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
            continue;
        }

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
                continue;
            }
            client->worker_id = idx;
            if (client_tls_handshake(srv, client) != 0) {
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }

            /* Hold clients_lock through idmap publish + ev_add so TEARDOWN /
               keepalive cannot destroy the client mid-accept (they need the
               same lock). epoll_ctl does not invoke callbacks. */
            cmq_mutex_lock(&w->clients_lock);
            if (w->clients_count >= w->clients_cap) {
                if (clients_array_grow(&w->clients, &w->clients_cap) != 0) {
                    cmq_mutex_unlock(&w->clients_lock);
                    cmq_client_destroy(client);
                    cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                    continue;
                }
            }
            w->clients[w->clients_count++] = client;
            if (cmq_idmap_put(w->idmap, client->id, client) != 0) {
                w->clients_count--;
                cmq_mutex_unlock(&w->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }
            if (cmq_ev_add(w->ev_loop, client_fd, CMQ_EV_READ, client_read_cb,
                           client) != 0) {
                w->clients_count--;
                cmq_idmap_del(w->idmap, client->id);
                cmq_mutex_unlock(&w->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }
            cmq_mutex_unlock(&w->clients_lock);
        } else {
            cmq_client_t *client = cmq_client_create(client_fd, cid,
                                                        srv->ev_loop, srv);
            if (!client) {
                close(client_fd);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }
            client->worker_id = -1;
            if (client_tls_handshake(srv, client) != 0) {
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }

            cmq_mutex_lock(&srv->clients_lock);
            if (srv->clients_count >= srv->clients_cap) {
                if (clients_array_grow(&srv->clients, &srv->clients_cap) != 0) {
                    cmq_mutex_unlock(&srv->clients_lock);
                    cmq_client_destroy(client);
                    cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                    continue;
                }
            }
            srv->clients[srv->clients_count++] = client;
            if (cmq_idmap_put(srv->idmap, client->id, client) != 0) {
                srv->clients_count--;
                cmq_mutex_unlock(&srv->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }
            if (cmq_ev_add(srv->ev_loop, client_fd, CMQ_EV_READ, client_read_cb,
                           client) != 0) {
                srv->clients_count--;
                cmq_idmap_del(srv->idmap, client->id);
                cmq_mutex_unlock(&srv->clients_lock);
                cmq_client_destroy(client);
                cmq_atomic_fetch_sub_u32(&srv->active_clients, 1, CMQ_ATOMIC_RELAXED);
                continue;
            }
            cmq_mutex_unlock(&srv->clients_lock);
        }
    }
}

const char *cmq_version(void) {
    return CMQ_VERSION_STRING;
}

cmq_status_t cmq_server_create(cmq_server_t **server, const cmq_config_t *config) {
    if (!server) return CMQ_ERR_INVALID_ARG;

    cmq_server_t *srv = calloc(1, sizeof(cmq_server_t));
    if (!srv) return CMQ_ERR_NO_MEMORY;

    cmq_config_t src = {0};
    if (config) src = *config;

    /* Copy scalars; strings are duplicated below so server owns them. */
    srv->config = src;
    srv->config.host = NULL;
    srv->config.log_file = NULL;
    srv->config.auth_username = NULL;
    srv->config.auth_password = NULL;
    srv->config.cluster_name = NULL;
    srv->config.cluster_node_id = NULL;
    srv->config.tls_cert = NULL;
    srv->config.tls_key = NULL;
    srv->config.route_count = 0;
    for (int i = 0; i < 8; i++) {
        srv->config.routes[i].addr = NULL;
        srv->config.routes[i].port = 0;
    }

    const char *host_src = src.host ? src.host : CMQ_DEFAULT_HOST;
    srv->config.host = strdup(host_src);
    if (!srv->config.host) {
        free(srv);
        return CMQ_ERR_NO_MEMORY;
    }
#define OWN(dst, srcv) do { \
        if ((srcv)) { \
            (dst) = strdup(srcv); \
            if (!(dst)) { cmq_config_free(&srv->config); free(srv); return CMQ_ERR_NO_MEMORY; } \
        } \
    } while (0)
    OWN(srv->config.log_file, src.log_file);
    OWN(srv->config.auth_username, src.auth_username);
    OWN(srv->config.auth_password, src.auth_password);
    OWN(srv->config.cluster_name, src.cluster_name);
    OWN(srv->config.cluster_node_id, src.cluster_node_id);
    OWN(srv->config.tls_cert, src.tls_cert);
    OWN(srv->config.tls_key, src.tls_key);
#undef OWN
    for (int i = 0; i < src.route_count && i < 8; i++) {
        if (!src.routes[i].addr) continue;
        char *a = strdup(src.routes[i].addr);
        if (!a) {
            cmq_config_free(&srv->config);
            free(srv);
            return CMQ_ERR_NO_MEMORY;
        }
        srv->config.routes[srv->config.route_count].addr = a;
        srv->config.routes[srv->config.route_count].port = src.routes[i].port;
        srv->config.route_count++;
    }

    if (srv->config.port == 0) srv->config.port = CMQ_DEFAULT_PORT;
    if (srv->config.max_payload_size == 0)
        srv->config.max_payload_size = CMQ_DEFAULT_MAX_PAYLOAD;
    if (srv->config.max_subs_per_client == 0)
        srv->config.max_subs_per_client = CMQ_DEFAULT_MAX_SUBS_PER_CLIENT;
    if (srv->config.ping_interval_ms == 0)
        srv->config.ping_interval_ms = CMQ_DEFAULT_PING_INTERVAL;
    if (srv->config.write_timeout_ms == 0)
        srv->config.write_timeout_ms = CMQ_DEFAULT_WRITE_TIMEOUT;

    if (cmq_config_validate(&srv->config) != CMQ_OK) {
        cmq_config_free(&srv->config);
        free(srv);
        return CMQ_ERR_INVALID_ARG;
    }

    srv->listen_fd = -1;
    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);

    cmq_mutex_init(&srv->clients_lock);
    cmq_rwlock_init(&srv->sublist_lock);

    srv->sublist = cmq_sublist_create();
    if (!srv->sublist) {
        cmq_mutex_destroy(&srv->clients_lock);
        cmq_rwlock_destroy(&srv->sublist_lock);
        cmq_config_free(&srv->config);
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
    srv->idmap = cmq_idmap_create(64);
    cmq_atomic_store_u32(&srv->next_client_id, 1, CMQ_ATOMIC_RELAXED);
    cmq_atomic_store_u32(&srv->next_conn_gen, 1, CMQ_ATOMIC_RELAXED);
    if (!srv->clients || !srv->idmap) {
        free(srv->clients);
        cmq_idmap_destroy(srv->idmap);
        cmq_sublist_destroy(srv->sublist);
        cmq_log_destroy(srv->log);
        cmq_mutex_destroy(&srv->clients_lock);
        cmq_rwlock_destroy(&srv->sublist_lock);
        cmq_config_free(&srv->config);
        free(srv);
        return CMQ_ERR_NO_MEMORY;
    }

    srv->accounts = cmq_account_manager_create();
    if (!srv->accounts ||
        cmq_account_create(srv->accounts, "$default") != 0) {
        if (srv->accounts) cmq_account_manager_destroy(srv->accounts);
        free(srv->clients);
        cmq_idmap_destroy(srv->idmap);
        cmq_sublist_destroy(srv->sublist);
        cmq_log_destroy(srv->log);
        cmq_mutex_destroy(&srv->clients_lock);
        cmq_rwlock_destroy(&srv->sublist_lock);
        cmq_config_free(&srv->config);
        free(srv);
        return CMQ_ERR_NO_MEMORY;
    }

    srv->routes = NULL;
    srv->cluster = NULL;
    srv->tls_config = NULL;

    if (srv->config.tls_enabled) {
        if (!srv->config.tls_cert || !srv->config.tls_key ||
            srv->config.tls_cert[0] == '\0' || srv->config.tls_key[0] == '\0') {
            cmq_log_error(srv->log,
                "TLS enabled but tls_cert/tls_key missing — refusing plaintext");
            cmq_server_destroy(srv);
            *server = NULL;
            return CMQ_ERR_INVALID_ARG;
        }
        if (!cmq_tls_backend_secure()) {
            cmq_log_error(srv->log,
                "TLS requested but no secure backend linked — refusing plaintext stub");
            cmq_server_destroy(srv);
            *server = NULL;
            return CMQ_ERR_INVALID_ARG;
        }
        srv->tls_config = cmq_tls_config_create();
        if (!srv->tls_config) {
            cmq_log_error(srv->log, "TLS config allocation failed — refusing plaintext");
            cmq_server_destroy(srv);
            *server = NULL;
            return CMQ_ERR_NO_MEMORY;
        }
        cmq_tls_set_cert(srv->tls_config, srv->config.tls_cert);
        cmq_tls_set_key(srv->tls_config, srv->config.tls_key);
        cmq_log_info(srv->log, "TLS enabled: cert=%s", srv->config.tls_cert);
    }

    if (srv->config.cluster_name && srv->config.cluster_node_id) {
        srv->cluster = cmq_cluster_create(srv->config.cluster_name,
                                           srv->config.cluster_node_id);
        if (srv->cluster) {
            srv->routes = cmq_route_pool_create(srv->cluster);
        }
    }
    /* Configured routes without a pool would silently no-op all cluster
       forwards (forward_op/missed treat NULL routes as success). */
    if (srv->config.route_count > 0 && !srv->routes) {
        cmq_server_destroy(srv);
        *server = NULL;
        return CMQ_ERR_NO_MEMORY;
    }

    *server = srv;
    return CMQ_OK;
}

cmq_status_t cmq_server_run(cmq_server_t *srv) {
    if (!srv) return CMQ_ERR_INVALID_ARG;
    /* Reject re-entry while a prior run's loop/workers still exist. */
    if (srv->listen_fd >= 0 || srv->ev_loop || srv->workers)
        return CMQ_ERR_INVALID_ARG;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) return CMQ_ERR_IO;

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (set_nonblocking(srv->listen_fd) != 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_IO;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)srv->config.port);
    if (!srv->config.host ||
        inet_pton(AF_INET, srv->config.host, &addr.sin_addr) != 1) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_INVALID_ARG;
    }

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_IO;
    }

    if (listen(srv->listen_fd, 512) != 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_IO;
    }

    srv->ev_loop = cmq_ev_loop_create(1024);
    if (!srv->ev_loop) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_NO_MEMORY;
    }

    if (cmq_ev_add(srv->ev_loop, srv->listen_fd, CMQ_EV_READ, accept_cb,
                   srv) != 0) {
        cmq_ev_loop_destroy(srv->ev_loop);
        srv->ev_loop = NULL;
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return CMQ_ERR_IO;
    }

    /* Acceptor-thread drain of local clients (single-thread / num_threads<=1). */
    cmq_ev_set_post_tick(srv->ev_loop, acceptor_post_tick, srv);

    if (srv->config.ping_interval_ms > 0) {
        if (cmq_ev_timer_add(srv->ev_loop, (uint64_t)srv->config.ping_interval_ms,
                              (uint64_t)srv->config.ping_interval_ms,
                              keepalive_timer_cb, srv) < 0) {
            cmq_log_error(srv->log, "keepalive timer registration failed");
            cmq_ev_loop_destroy(srv->ev_loop);
            srv->ev_loop = NULL;
            close(srv->listen_fd);
            srv->listen_fd = -1;
            return CMQ_ERR_IO;
        }
    }

    int nthreads = srv->config.num_threads;
    if (nthreads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        nthreads = (int)n;
        srv->config.num_threads = nthreads;
    } else if (nthreads > 64) {
        nthreads = 64;
        srv->config.num_threads = nthreads;
    }
    if (nthreads > 1) {
        srv->num_workers = nthreads;
        srv->workers = calloc((size_t)nthreads, sizeof(cmq_worker_t));
        if (!srv->workers) {
            close(srv->listen_fd);
            srv->listen_fd = -1;
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
                srv->listen_fd = -1;
                return CMQ_ERR_NO_MEMORY;
            }
            w->wakeup_fd = -1;
            w->wakeup_wfd = -1;
            if (wakeup_fd_pair(&w->wakeup_fd, &w->wakeup_wfd) != 0) {
                cmq_ev_loop_destroy(w->ev_loop);
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return CMQ_ERR_NO_MEMORY;
            }
            if (cmq_ev_add(w->ev_loop, w->wakeup_fd, CMQ_EV_READ,
                           worker_wakeup_cb, w) != 0) {
                wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
                cmq_ev_loop_destroy(w->ev_loop);
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return CMQ_ERR_IO;
            }
            w->clients_cap = 64;
            w->clients_count = 0;
            w->clients = calloc((size_t)w->clients_cap, sizeof(cmq_client_t *));
            w->idmap = cmq_idmap_create(64);
            if (!w->clients || !w->idmap) {
                free(w->clients);
                cmq_idmap_destroy(w->idmap);
                w->clients = NULL;
                w->idmap = NULL;
                cmq_ev_loop_destroy(w->ev_loop);
                wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return CMQ_ERR_NO_MEMORY;
            }
            cmq_mutex_init(&w->clients_lock);
            cmq_mutex_init(&w->msg_lock);
            w->msg_head = NULL;
            w->msg_tail = NULL;
            w->coro_cap = CMQ_CORO_MAX_PER_WORKER;
            w->coro_count = 0;
            w->coro_pool = calloc((size_t)w->coro_cap, sizeof(cmq_coro_t *));
            if (!w->coro_pool) {
                cmq_mutex_destroy(&w->msg_lock);
                cmq_mutex_destroy(&w->clients_lock);
                free(w->clients);
                cmq_idmap_destroy(w->idmap);
                w->clients = NULL;
                w->idmap = NULL;
                cmq_ev_loop_destroy(w->ev_loop);
                wakeup_fd_close(w->wakeup_fd, w->wakeup_wfd);
                for (int j = 0; j < i; j++) {
                    cmq_ev_stop(srv->workers[j].ev_loop);
                    cmq_worker_destroy(&srv->workers[j]);
                }
                free(srv->workers);
                srv->workers = NULL;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return CMQ_ERR_NO_MEMORY;
            }
        }
        for (int i = 0; i < nthreads; i++) {
            if (cmq_thread_create(&srv->workers[i].thread, worker_thread,
                                  &srv->workers[i]) != 0) {
                cmq_log_error(srv->log, "Failed to create worker thread %d", i);
                for (int j = 0; j < i; j++)
                    cmq_ev_stop(srv->workers[j].ev_loop);
                for (int j = 0; j < i; j++)
                    cmq_thread_join(srv->workers[j].thread);
                for (int j = 0; j < nthreads; j++)
                    cmq_worker_destroy(&srv->workers[j]);
                free(srv->workers);
                srv->workers = NULL;
                srv->num_workers = 0;
                cmq_ev_loop_destroy(srv->ev_loop);
                srv->ev_loop = NULL;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return CMQ_ERR_IO;
            }
        }
        srv->workers_joinable = nthreads;
        cmq_log_info(srv->log, "CMQ server started with %d worker threads", nthreads);
    }

    cmq_atomic_store_int(&srv->running, 1, CMQ_ATOMIC_SEQ_CST);
    cmq_log_info(srv->log, "CMQ server listening on %s:%d",
                 srv->config.host, srv->config.port);

    if (srv->routes && srv->config.route_count > 0) {
        for (int i = 0; i < srv->config.route_count; i++) {
            /* Unique per peer — shared "node-<port>" skipped all but first. */
            char nid[CMQ_NODE_ID_SIZE];
            snprintf(nid, sizeof(nid), "r%d", i);
            if (cmq_route_connect(srv->routes, nid,
                                  srv->config.routes[i].addr,
                                  srv->config.routes[i].port,
                                  srv->config.auth_username,
                                  srv->config.auth_password) == 0) {
                cmq_log_info(srv->log, "Route connected to %s:%d",
                             srv->config.routes[i].addr,
                             srv->config.routes[i].port);
            } else {
                cmq_log_warn(srv->log, "Route connect failed to %s:%d",
                             srv->config.routes[i].addr,
                             srv->config.routes[i].port);
            }
        }
        /* Background reconnect — never block acceptor keepalive/accept. */
        if (cmq_thread_create(&srv->route_reconn_thr, route_reconnect_thread,
                               srv) == 0)
            srv->route_reconn_started = 1;
        else
            cmq_log_warn(srv->log, "Route reconnect thread failed to start");
    }

    cmq_ev_run(srv->ev_loop, -1);

    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);
    /* Claim+join — destroy may race; only one caller joins. */
    if (__atomic_exchange_n(&srv->route_reconn_started, 0, __ATOMIC_ACQ_REL))
        cmq_thread_join(srv->route_reconn_thr);

    /* Claim+join workers — destroy may race; only one caller joins. */
    {
        int njoin = __atomic_exchange_n(&srv->workers_joinable, 0,
                                         __ATOMIC_ACQ_REL);
        if (njoin > 0 && srv->workers) {
            for (int i = 0; i < srv->num_workers; i++)
                cmq_ev_stop(srv->workers[i].ev_loop);
            for (int i = 0; i < njoin && i < srv->num_workers; i++)
                cmq_thread_join(srv->workers[i].thread);
        }
    }

    /* Free the bind port; ev_loop/workers stay until destroy (clients still use them). */
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    return CMQ_OK;
}

/* Run only on the acceptor event-loop thread (via post_tick after wakeup). */
static void acceptor_process_drain(cmq_server_t *srv) {
    if (!srv || !cmq_atomic_load_int(&srv->acceptor_drain, CMQ_ATOMIC_ACQUIRE))
        return;

    uint8_t disc[16];
    size_t disc_len = cmq_frame_encode(disc, sizeof(disc), CMQ_OP_DISCONNECT, 0, NULL, 0);

    cmq_mutex_lock(&srv->clients_lock);
    int nacc = srv->clients_count;
    cmq_client_t **acc_snap = NULL;
    if (nacc > 0) {
        acc_snap = malloc((size_t)nacc * sizeof(cmq_client_t *));
        if (acc_snap) {
            int n = 0;
            for (int i = 0; i < nacc; i++) {
                cmq_client_t *c = srv->clients[i];
                if (!c || c->state == CMQ_CLIENT_CLOSED)
                    continue;
                /* DISCONNECT while still CONNECTED — send_local rejects CLOSING. */
                if (c->state == CMQ_CLIENT_CONNECTED && disc_len > 0)
                    (void)cmq_client_send_local(c, disc, disc_len);
                if (c->state == CMQ_CLIENT_CONNECTED ||
                    c->state == CMQ_CLIENT_INIT)
                    c->state = CMQ_CLIENT_CLOSING;
                /* Pre-existing CLOSING: finish below. */
                acc_snap[n++] = c;
            }
            nacc = n;
        } else {
            /* OOM: nudge EOF only — no send_local under clients_lock. */
            for (int i = 0; i < nacc; i++) {
                cmq_client_t *c = srv->clients[i];
                if (c && c->fd >= 0 &&
                    (c->state == CMQ_CLIENT_CONNECTED ||
                     c->state == CMQ_CLIENT_INIT ||
                     c->state == CMQ_CLIENT_CLOSING)) {
                    if (c->state != CMQ_CLIENT_CLOSING &&
                        c->state != CMQ_CLIENT_CLOSED)
                        c->state = CMQ_CLIENT_CLOSING;
                    (void)shutdown(c->fd, SHUT_RDWR);
                }
            }
            nacc = 0;
        }
    }
    cmq_mutex_unlock(&srv->clients_lock);
    if (acc_snap) {
        for (int i = 0; i < nacc; i++) {
            cmq_client_t *c = acc_snap[i];
            if (c && c->state == CMQ_CLIENT_CLOSING)
                client_finish_closing(c);
        }
        free(acc_snap);
    }
    /* Do not clear acceptor_drain here: srv->clients_count==0 can be true in
       worker-only mode while workers still hold clients. Drain waiter clears. */
}

static void acceptor_post_tick(void *data) {
    acceptor_process_drain((cmq_server_t *)data);
}

void cmq_server_drain(cmq_server_t *srv, int drain_timeout_ms) {
    if (!srv) return;

    /* Raise drain before closing listen so concurrent accept_cb rejects. */
    cmq_atomic_store_int(&srv->acceptor_drain, 1, CMQ_ATOMIC_RELEASE);
    if (srv->ev_loop)
        cmq_ev_wakeup(srv->ev_loop);

    if (srv->listen_fd >= 0) {
        cmq_ev_del(srv->ev_loop, srv->listen_fd);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    uint8_t disc[16];
    size_t disc_len = cmq_frame_encode(disc, sizeof(disc), CMQ_OP_DISCONNECT, 0, NULL, 0);

    /* Acceptor-owned clients live on srv->ev_loop — never send_local /
       finish_closing from this (possibly foreign) thread. */

    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_worker_t *w = &srv->workers[i];
            cmq_mutex_lock(&w->clients_lock);
            int wn = w->clients_count;
            typedef struct { uint32_t id; uint32_t gen; uint8_t send_disc; } cmq_drain_key_t;
            cmq_drain_key_t *keys = NULL;
            int nids = 0;
            if (wn > 0) {
                keys = malloc((size_t)wn * sizeof(cmq_drain_key_t));
                if (keys) {
                    for (int j = 0; j < wn; j++) {
                        cmq_client_t *c = w->clients[j];
                        if (!c || c->state == CMQ_CLIENT_CLOSED)
                            continue;
                        if (c->state == CMQ_CLIENT_CONNECTED ||
                            c->state == CMQ_CLIENT_INIT ||
                            c->state == CMQ_CLIENT_CLOSING) {
                            keys[nids].id = c->id;
                            keys[nids].gen = c->conn_gen;
                            keys[nids].send_disc =
                                (c->state == CMQ_CLIENT_CONNECTED) ? 1 : 0;
                            nids++;
                        }
                    }
                } else {
                    for (int j = 0; j < wn; j++) {
                        cmq_client_t *c = w->clients[j];
                        if (c && c->fd >= 0 &&
                            (c->state == CMQ_CLIENT_CONNECTED ||
                             c->state == CMQ_CLIENT_INIT ||
                             c->state == CMQ_CLIENT_CLOSING))
                            shutdown(c->fd, SHUT_RDWR);
                    }
                }
            }
            cmq_mutex_unlock(&w->clients_lock);
            for (int j = 0; j < nids; j++) {
                if (keys[j].send_disc && disc_len > 0)
                    worker_push_msg(w, keys[j].id, keys[j].gen, disc, disc_len, 0, 0, NULL, 0, 0,
                                    NULL, 0, NULL, 0);
                if (worker_push_teardown(w, keys[j].id, keys[j].gen) != 0) {
                    /* OOM: force-close fd so the worker notices EOF (same as
                       keepalive_timer_cb). */
                    cmq_mutex_lock(&w->clients_lock);
                    for (int k = 0; k < w->clients_count; k++) {
                        cmq_client_t *c = w->clients[k];
                        if (c && c->id == keys[j].id &&
                            c->conn_gen == keys[j].gen && c->fd >= 0) {
                            shutdown(c->fd, SHUT_RDWR);
                            break;
                        }
                    }
                    cmq_mutex_unlock(&w->clients_lock);
                }
            }
            free(keys);
        }
    }

    int timeout = drain_timeout_ms > 0 ? drain_timeout_ms : 0;
    uint64_t start = srv_now_ms();
    for (;;) {
        int n = 0;
        cmq_mutex_lock(&srv->clients_lock);
        n += srv->clients_count;
        cmq_mutex_unlock(&srv->clients_lock);
        if (srv->workers) {
            for (int i = 0; i < srv->num_workers; i++) {
                cmq_mutex_lock(&srv->workers[i].clients_lock);
                n += srv->workers[i].clients_count;
                cmq_mutex_unlock(&srv->workers[i].clients_lock);
            }
        }
        if (n == 0) {
            cmq_atomic_store_int(&srv->acceptor_drain, 0, CMQ_ATOMIC_RELEASE);
            break;
        }
        if ((int)(srv_now_ms() - start) >= timeout) {
            cmq_atomic_store_int(&srv->acceptor_drain, 0, CMQ_ATOMIC_RELEASE);
            break;
        }
        /* Re-kick acceptor drain in case a late accept slipped in. */
        if (srv->ev_loop &&
            cmq_atomic_load_int(&srv->acceptor_drain, CMQ_ATOMIC_ACQUIRE))
            cmq_ev_wakeup(srv->ev_loop);
        struct timespec ts = {0, 1000000L}; /* 1ms */
        nanosleep(&ts, NULL);
    }

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

    /* Stop reconn before tearing down routes (connect unlocks mid-dial). */
    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);
    if (srv->ev_loop) cmq_ev_stop(srv->ev_loop);
    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++)
            cmq_ev_stop(srv->workers[i].ev_loop);
    }
    if (__atomic_exchange_n(&srv->route_reconn_started, 0, __ATOMIC_ACQ_REL))
        cmq_thread_join(srv->route_reconn_thr);
    {
        int njoin = __atomic_exchange_n(&srv->workers_joinable, 0,
                                         __ATOMIC_ACQ_REL);
        if (njoin > 0 && srv->workers) {
            for (int i = 0; i < njoin && i < srv->num_workers; i++)
                cmq_thread_join(srv->workers[i].thread);
        }
    }

    if (srv->workers) {
        for (int i = 0; i < srv->num_workers; i++) {
            cmq_worker_destroy(&srv->workers[i]);
        }
        free(srv->workers);
    }

    if (srv->clients) {
        while (srv->clients_count > 0)
            client_teardown(srv->clients[0]);
        free(srv->clients);
        srv->clients = NULL;
    }
    cmq_idmap_destroy(srv->idmap);
    srv->idmap = NULL;

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
    cmq_config_free(&srv->config);
    free(srv);
}
