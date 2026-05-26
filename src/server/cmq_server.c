#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_platform.h"

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void client_read_cb(int fd, int events, void *data);

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
    c->next = NULL;
    return c;
}

static void cmq_client_destroy(cmq_client_t *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    if (c->parser) cmq_parser_destroy(c->parser);
    free(c->write_buf);
    cmq_sub_entry_t *s = c->subs;
    while (s) {
        cmq_sub_entry_t *next = s->next;
        free(s);
        s = next;
    }
    free(c);
}

static int cmq_client_send(cmq_client_t *c, const uint8_t *data, size_t len) {
    if (!c || c->state == CMQ_CLIENT_CLOSED) return -1;

    if (c->write_buf && c->write_pos < c->write_len) {
        size_t remaining = c->write_len - c->write_pos;
        size_t new_len = remaining + len;
        uint8_t *new_buf = malloc(new_len);
        if (!new_buf) return -1;
        memcpy(new_buf, c->write_buf + c->write_pos, remaining);
        memcpy(new_buf + remaining, data, len);
        free(c->write_buf);
        c->write_buf = new_buf;
        c->write_len = new_len;
        c->write_pos = 0;
        return 0;
    }

    free(c->write_buf);
    c->write_buf = malloc(len);
    if (!c->write_buf) return -1;
    memcpy(c->write_buf, data, len);
    c->write_len = len;
    c->write_pos = 0;

    cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ | CMQ_EV_WRITE, client_read_cb, c);
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

static void cmq_send_message(cmq_client_t *c, uint32_t sub_id,
                              const char *subject,
                              const uint8_t *payload, size_t payload_len) {
    size_t subject_len = strlen(subject);
    size_t body_len = 4 + 2 + subject_len + 4 + payload_len;
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
} cmq_sub_ref_t;

static void handle_publish(cmq_server_t *srv, cmq_client_t *c,
                            const cmq_frame_t *frame) {
    (void)c;
    if (!frame->payload || frame->payload_len < 2) {
        cmq_send_error(c, "invalid publish");
        return;
    }

    uint16_t subject_len = ((uint16_t)frame->payload[0] << 8) | frame->payload[1];
    if (2 + subject_len > frame->payload_len) {
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

    const uint8_t *msg_payload = frame->payload + offset;
    size_t msg_len = frame->payload_len - offset;

    cmq_rwlock_rdlock(&srv->sublist_lock);
    cmq_sublist_result_t result;
    cmq_sublist_match(srv->sublist, subject, &result);

    for (size_t i = 0; i < result.count; i++) {
        cmq_sub_ref_t *ref = (cmq_sub_ref_t *)result.entries[i];
        cmq_send_message(ref->client, ref->sub_id, subject,
                          msg_payload, msg_len);
    }
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
    if (6 + subject_len > frame->payload_len || subject_len >= CMQ_MAX_SUBJECT) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    char subject[CMQ_MAX_SUBJECT];
    memcpy(subject, frame->payload + 6, subject_len);
    subject[subject_len] = '\0';

    cmq_sub_entry_t *entry = malloc(sizeof(cmq_sub_entry_t));
    if (!entry) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    entry->sub_id = sub_id;
    strncpy(entry->subject, subject, CMQ_MAX_SUBJECT - 1);
    entry->subject[CMQ_MAX_SUBJECT - 1] = '\0';
    entry->next = c->subs;
    c->subs = entry;

    cmq_sub_ref_t *ref = malloc(sizeof(cmq_sub_ref_t));
    if (!ref) {
        cmq_send_suback(c, sub_id, 1);
        return;
    }
    ref->client = c;
    ref->sub_id = sub_id;

    cmq_rwlock_wrlock(&srv->sublist_lock);
    cmq_sublist_insert(srv->sublist, subject, ref);
    cmq_rwlock_unlock(&srv->sublist_lock);

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
            cmq_sublist_remove(srv->sublist, entry->subject);
            cmq_rwlock_unlock(&srv->sublist_lock);

            free(entry);
            break;
        }
        pp = &(*pp)->next;
    }

    uint8_t ack[16];
    size_t len = cmq_frame_encode(ack, sizeof(ack), CMQ_OP_UNSUBACK, 0,
                                   frame->payload, 4);
    if (len > 0) cmq_client_send(c, ack, len);
}

static void handle_frame(cmq_server_t *srv, cmq_client_t *c,
                          const cmq_frame_t *frame) {
    switch (frame->hdr.op) {
    case CMQ_OP_CONNECT:
        c->state = CMQ_CLIENT_CONNECTED;
        cmq_send_connack(c, 0);
        break;
    case CMQ_OP_PING:
        cmq_send_pong(c);
        break;
    case CMQ_OP_PUBLISH:
        handle_publish(srv, c, frame);
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
    default:
        cmq_send_error(c, "unknown op");
        break;
    }
}

static void client_flush_write(cmq_client_t *c) {
    if (!c->write_buf || c->write_pos >= c->write_len) {
        free(c->write_buf);
        c->write_buf = NULL;
        c->write_len = 0;
        c->write_pos = 0;
        cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c);
        return;
    }

    size_t remaining = c->write_len - c->write_pos;
    ssize_t n = write(c->fd, c->write_buf + c->write_pos, remaining);
    if (n > 0) {
        c->write_pos += (size_t)n;
        if (c->write_pos >= c->write_len) {
            free(c->write_buf);
            c->write_buf = NULL;
            c->write_len = 0;
            c->write_pos = 0;
            cmq_ev_mod(c->ev_loop, c->fd, CMQ_EV_READ, client_read_cb, c);
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        c->state = CMQ_CLIENT_CLOSED;
    }
}

static void client_read_cb(int fd, int events, void *data) {
    cmq_client_t *c = (cmq_client_t *)data;
    cmq_server_t *srv = c->server;

    if (events & CMQ_EV_ERROR) {
        c->state = CMQ_CLIENT_CLOSED;
        return;
    }

    if (events & CMQ_EV_WRITE) {
        client_flush_write(c);
        if (c->state == CMQ_CLIENT_CLOSED) return;
    }

    if (!(events & CMQ_EV_READ)) return;

    ssize_t n = read(fd, c->read_buf, sizeof(c->read_buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            c->state = CMQ_CLIENT_CLOSED;
        }
        return;
    }

    int rc = cmq_parser_feed(c->parser, c->read_buf, (size_t)n);
    if (rc < 0) {
        c->state = CMQ_CLIENT_CLOSING;
        return;
    }

    while (rc == 1) {
        const cmq_frame_t *frame = cmq_parser_frame(c->parser);
        if (frame) {
            handle_frame(srv, c, frame);
        }
        if (c->state == CMQ_CLIENT_CLOSING || c->state == CMQ_CLIENT_CLOSED) break;
        rc = cmq_parser_next(c->parser);
    }
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

    uint32_t cid = cmq_atomic_fetch_add_u32(&srv->next_client_id, 1,
                                             CMQ_ATOMIC_SEQ_CST);
    cmq_client_t *client = cmq_client_create(client_fd, cid,
                                              srv->ev_loop, srv);
    if (!client) {
        close(client_fd);
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
            return;
        }
        srv->clients = new_arr;
        srv->clients_cap = new_cap;
    }
    srv->clients[srv->clients_count++] = client;
    cmq_mutex_unlock(&srv->clients_lock);

    cmq_ev_add(srv->ev_loop, client_fd, CMQ_EV_READ, client_read_cb, client);

    uint8_t info_buf[128];
    const char *info = "{\"version\":\"0.1.0\",\"proto\":1}";
    size_t len = cmq_frame_encode(info_buf, sizeof(info_buf), CMQ_OP_INFO, 0,
                                   (const uint8_t *)info, strlen(info));
    if (len > 0) cmq_client_send(client, info_buf, len);
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

    cmq_atomic_store_int(&srv->running, 1, CMQ_ATOMIC_SEQ_CST);
    cmq_log_info(srv->log, "CMQ server listening on %s:%d",
                 srv->config.host, srv->config.port);

    cmq_ev_run(srv->ev_loop, -1);

    return CMQ_OK;
}

void cmq_server_stop(cmq_server_t *srv) {
    if (!srv) return;
    cmq_atomic_store_int(&srv->running, 0, CMQ_ATOMIC_SEQ_CST);
    if (srv->ev_loop) cmq_ev_stop(srv->ev_loop);
}

void cmq_server_destroy(cmq_server_t *srv) {
    if (!srv) return;

    if (srv->clients) {
        for (int i = 0; i < srv->clients_count; i++) {
            cmq_client_destroy(srv->clients[i]);
        }
        free(srv->clients);
    }

    if (srv->listen_fd >= 0) close(srv->listen_fd);
    if (srv->ev_loop) cmq_ev_loop_destroy(srv->ev_loop);
    if (srv->sublist) cmq_sublist_destroy(srv->sublist);
    if (srv->log) cmq_log_destroy(srv->log);
    cmq_mutex_destroy(&srv->clients_lock);
    cmq_rwlock_destroy(&srv->sublist_lock);
    free(srv);
}
