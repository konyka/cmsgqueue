#define _POSIX_C_SOURCE 200809L
#include "cmq_mqtt.h"
#include "cmq_route.h"
#include "cmq_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <time.h>

#define CMQ_MQTT_MAX_MAPPINGS 64
#define CMQ_MQTT_CONNECT_MS   2000
#define CMQ_MQTT_IO_MS        2000

#define CMQ_MQTT_CONNECT    0x10
#define CMQ_MQTT_CONNACK    0x20
#define CMQ_MQTT_PUBLISH    0x30
#define CMQ_MQTT_SUBSCRIBE  0x80
#define CMQ_MQTT_PINGREQ    0xC0

struct cmq_mqtt_bridge {
    char client_id[CMQ_MQTT_CLIENT_ID];
    char addr[64];
    int port;
    int fd;
    int connected;
    int keepalive_ms;
    int clean_session;
    uint64_t messages_in;
    uint64_t messages_out;
    cmq_mqtt_mapping_t mappings[CMQ_MQTT_MAX_MAPPINGS];
    size_t mapping_count;
    cmq_mutex_t lock;
    atomic_int in_flight; /* connect/is_connected unlocked dial/probe */
    atomic_int dying;
};

static int mqtt_begin_op(cmq_mqtt_bridge_t *br) {
    if (atomic_load_explicit(&br->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&br->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&br->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&br->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void mqtt_end_op(cmq_mqtt_bridge_t *br) {
    atomic_fetch_sub_explicit(&br->in_flight, 1, memory_order_acq_rel);
}

static int mqtt_fd_alive(int fd) {
    return cmq_tcp_fd_alive(fd);
}

static void mqtt_disconnect_unlocked(cmq_mqtt_bridge_t *br) {
    if (br->fd >= 0) close(br->fd);
    br->fd = -1;
    br->connected = 0;
}

cmq_mqtt_bridge_t *cmq_mqtt_bridge_create(const char *client_id) {
    if (!client_id) return NULL;
    if (strnlen(client_id, CMQ_MQTT_CLIENT_ID) >= CMQ_MQTT_CLIENT_ID)
        return NULL;
    cmq_mqtt_bridge_t *br = calloc(1, sizeof(cmq_mqtt_bridge_t));
    if (!br) return NULL;
    snprintf(br->client_id, sizeof(br->client_id), "%s", client_id);
    br->fd = -1;
    br->keepalive_ms = 60000;
    br->clean_session = 1;
    atomic_init(&br->in_flight, 0);
    atomic_init(&br->dying, 0);
    cmq_mutex_init(&br->lock);
    return br;
}

void cmq_mqtt_bridge_destroy(cmq_mqtt_bridge_t *br) {
    if (!br) return;
    atomic_store_explicit(&br->dying, 1, memory_order_release);
    while (atomic_load_explicit(&br->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    cmq_mutex_lock(&br->lock);
    mqtt_disconnect_unlocked(br);
    cmq_mutex_unlock(&br->lock);
    cmq_mutex_destroy(&br->lock);
    free(br);
}

static void mqtt_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int mqtt_write_all(int fd, const uint8_t *data, size_t len) {
    size_t off = 0;
    int waited = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                for (;;) {
                    int pr = poll(&pfd, 1, 100);
                    if (pr > 0) break;
                    if (pr < 0 && errno == EINTR) continue;
                    waited += 100;
                    if (pr == 0 && waited < CMQ_MQTT_IO_MS) continue;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
        waited = 0;
    }
    return 0;
}

static int mqtt_read_connack(int fd) {
    uint8_t buf[8];
    size_t got = 0;
    int waited = 0;
    while (got < 4 && waited < 3000) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            waited += 100;
            continue;
        }
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        got += (size_t)n;
    }
    if (got < 4) return -1;
    return cmq_mqtt_decode_connack(buf, got);
}

int cmq_mqtt_bridge_connect(cmq_mqtt_bridge_t *br, const char *addr, int port) {
    if (!br || !addr) return -1;
    if (strnlen(addr, sizeof(br->addr)) >= sizeof(br->addr)) return -1;
    if (mqtt_begin_op(br) != 0) return -1;
    cmq_mutex_lock(&br->lock);
    /* Sticky only for the same endpoint — addr/port change must reconnect. */
    if (br->connected) {
        int efd = br->fd, ep = br->port;
        char ea[sizeof(br->addr)];
        strncpy(ea, br->addr, sizeof(ea) - 1);
        ea[sizeof(ea) - 1] = '\0';
        cmq_mutex_unlock(&br->lock);
        int alive = (efd >= 0 && ep == port &&
                     strncmp(ea, addr, sizeof(ea)) == 0 &&
                     mqtt_fd_alive(efd));
        cmq_mutex_lock(&br->lock);
        /* Re-check identity after probe — fd recycle must not fake success. */
        if (alive && br->connected && br->fd == efd && br->port == ep &&
            strncmp(br->addr, ea, sizeof(br->addr)) == 0) {
            cmq_mutex_unlock(&br->lock);
            mqtt_end_op(br);
            return 0;
        }
        /* Only drop still-dead probed fd — fd recycle must not kill a new peer. */
        if (br->fd == efd && !mqtt_fd_alive(efd))
            mqtt_disconnect_unlocked(br);
    }
    int keepalive_ms = br->keepalive_ms;
    int clean_session = br->clean_session;
    char client_id[CMQ_MQTT_CLIENT_ID];
    memcpy(client_id, br->client_id, sizeof(client_id));
    cmq_mutex_unlock(&br->lock);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        mqtt_end_op(br);
        return -1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        mqtt_end_op(br);
        return -1;
    }

    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_MQTT_CONNECT_MS) != 0) {
        close(fd);
        mqtt_end_op(br);
        return -1;
    }
    mqtt_set_nonblock(fd);

    uint8_t cbuf[256];
    int keepalive_s = keepalive_ms > 0 ? keepalive_ms / 1000 : 60;
    if (keepalive_s < 1) keepalive_s = 1;
    int clen = cmq_mqtt_encode_connect(cbuf, sizeof(cbuf), client_id,
                                        keepalive_s, clean_session);
    if (clen < 0 || mqtt_write_all(fd, cbuf, (size_t)clen) != 0 ||
        mqtt_read_connack(fd) != 0) {
        close(fd);
        mqtt_end_op(br);
        return -1;
    }

    cmq_mutex_lock(&br->lock);
    if (br->connected && br->fd >= 0) {
        int efd = br->fd;
        cmq_mutex_unlock(&br->lock);
        /* Probe — a dead sticky peer must not win over this fresh dial. */
        int alive = mqtt_fd_alive(efd);
        cmq_mutex_lock(&br->lock);
        if (alive && br->connected && br->fd == efd) {
            cmq_mutex_unlock(&br->lock);
            close(fd);
            mqtt_end_op(br);
            return 0;
        }
        if (br->fd == efd && !mqtt_fd_alive(efd))
            mqtt_disconnect_unlocked(br);
        else if (br->connected && br->fd >= 0) {
            /* Another connect installed a different live peer. */
            cmq_mutex_unlock(&br->lock);
            close(fd);
            mqtt_end_op(br);
            return 0;
        }
    }
    mqtt_disconnect_unlocked(br);
    br->fd = fd;
    br->connected = 1;
    snprintf(br->addr, sizeof(br->addr), "%s", addr);
    br->port = port;
    cmq_mutex_unlock(&br->lock);
    mqtt_end_op(br);
    return 0;
}

static int mqtt_bridge_disconnect_impl(cmq_mqtt_bridge_t *br) {
    if (!br) return -1;
    cmq_mutex_lock(&br->lock);
    mqtt_disconnect_unlocked(br);
    cmq_mutex_unlock(&br->lock);
    return 0;
}

int cmq_mqtt_bridge_is_connected(cmq_mqtt_bridge_t *br) {
    if (!br) return 0;
    if (mqtt_begin_op(br) != 0) return 0;
    cmq_mutex_lock(&br->lock);
    if (!br->connected || br->fd < 0) {
        cmq_mutex_unlock(&br->lock);
        mqtt_end_op(br);
        return 0;
    }
    int efd = br->fd;
    cmq_mutex_unlock(&br->lock);
    int alive = mqtt_fd_alive(efd);
    cmq_mutex_lock(&br->lock);
    if (alive && br->connected && br->fd == efd) {
        cmq_mutex_unlock(&br->lock);
        mqtt_end_op(br);
        return 1;
    }
    /* Dead sticky only — re-probe so fd recycle cannot kill a new peer. */
    if (br->fd == efd && !mqtt_fd_alive(efd))
        mqtt_disconnect_unlocked(br);
    cmq_mutex_unlock(&br->lock);
    mqtt_end_op(br);
    return 0;
}

const char *cmq_mqtt_client_id(cmq_mqtt_bridge_t *br) {
    if (!br || atomic_load_explicit(&br->dying, memory_order_acquire))
        return NULL;
    return br->client_id;
}

static cmq_mqtt_bridge_info_t mqtt_bridge_info_impl(cmq_mqtt_bridge_t *br) {
    cmq_mqtt_bridge_info_t info = {0};
    if (!br) return info;
    cmq_mutex_lock(&br->lock);
    strncpy(info.client_id, br->client_id, CMQ_MQTT_CLIENT_ID - 1);
    strncpy(info.addr, br->addr, sizeof(info.addr) - 1);
    info.port = br->port;
    info.keepalive_ms = br->keepalive_ms;
    info.clean_session = br->clean_session;
    info.connected = br->connected;
    info.messages_in = br->messages_in;
    info.messages_out = br->messages_out;
    cmq_mutex_unlock(&br->lock);
    return info;
}

static int mqtt_add_mapping_impl(cmq_mqtt_bridge_t *br, const char *cmq_subject,
                          const char *mqtt_topic, int qos) {
    if (!br || !cmq_subject || !mqtt_topic) return -1;
    if (qos < 0 || qos > 2) return -1;
    if (strnlen(cmq_subject, CMQ_MQTT_TOPIC_MAX) >= CMQ_MQTT_TOPIC_MAX)
        return -1;
    if (strnlen(mqtt_topic, CMQ_MQTT_TOPIC_MAX) >= CMQ_MQTT_TOPIC_MAX)
        return -1;
    cmq_mutex_lock(&br->lock);
    if (br->mapping_count >= CMQ_MQTT_MAX_MAPPINGS) {
        cmq_mutex_unlock(&br->lock);
        return -1;
    }
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0) {
            strncpy(br->mappings[i].cmq_subject, cmq_subject,
                    CMQ_MQTT_TOPIC_MAX - 1);
            br->mappings[i].cmq_subject[CMQ_MQTT_TOPIC_MAX - 1] = '\0';
            br->mappings[i].qos = qos;
            cmq_mutex_unlock(&br->lock);
            return 0;
        }
    }
    cmq_mqtt_mapping_t *m = &br->mappings[br->mapping_count++];
    strncpy(m->cmq_subject, cmq_subject, CMQ_MQTT_TOPIC_MAX - 1);
    m->cmq_subject[CMQ_MQTT_TOPIC_MAX - 1] = '\0';
    strncpy(m->mqtt_topic, mqtt_topic, CMQ_MQTT_TOPIC_MAX - 1);
    m->mqtt_topic[CMQ_MQTT_TOPIC_MAX - 1] = '\0';
    m->qos = qos;
    m->active = 1;
    cmq_mutex_unlock(&br->lock);
    return 0;
}

static int mqtt_remove_mapping_impl(cmq_mqtt_bridge_t *br, const char *mqtt_topic) {
    if (!br || !mqtt_topic) return -1;
    cmq_mutex_lock(&br->lock);
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0) {
            memmove(&br->mappings[i], &br->mappings[i + 1],
                    (br->mapping_count - i - 1) * sizeof(cmq_mqtt_mapping_t));
            br->mapping_count--;
            cmq_mutex_unlock(&br->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&br->lock);
    return -1;
}

static size_t mqtt_mapping_count_impl(cmq_mqtt_bridge_t *br) {
    if (!br) return 0;
    cmq_mutex_lock(&br->lock);
    size_t c = br->mapping_count;
    cmq_mutex_unlock(&br->lock);
    return c;
}

static int mqtt_find_mapping_impl(cmq_mqtt_bridge_t *br, const char *mqtt_topic,
                           cmq_mqtt_mapping_t *out) {
    if (!br || !mqtt_topic || !out) return -1;
    cmq_mutex_lock(&br->lock);
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0) {
            *out = br->mappings[i];
            cmq_mutex_unlock(&br->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&br->lock);
    return -1;
}


int cmq_mqtt_bridge_disconnect(cmq_mqtt_bridge_t *br) {
    if (!br) return -1;
    if (mqtt_begin_op(br) != 0) return -1;
    int rc = mqtt_bridge_disconnect_impl(br);
    mqtt_end_op(br);
    return rc;
}

cmq_mqtt_bridge_info_t cmq_mqtt_bridge_info(cmq_mqtt_bridge_t *br) {
    cmq_mqtt_bridge_info_t info = {0};
    if (!br) return info;
    if (mqtt_begin_op(br) != 0) return info;
    info = mqtt_bridge_info_impl(br);
    mqtt_end_op(br);
    return info;
}

int cmq_mqtt_add_mapping(cmq_mqtt_bridge_t *br, const char *cmq_subject,
                          const char *mqtt_topic, int qos) {
    if (!br || !cmq_subject || !mqtt_topic) return -1;
    if (mqtt_begin_op(br) != 0) return -1;
    int rc = mqtt_add_mapping_impl(br, cmq_subject, mqtt_topic, qos);
    mqtt_end_op(br);
    return rc;
}

int cmq_mqtt_remove_mapping(cmq_mqtt_bridge_t *br, const char *mqtt_topic) {
    if (!br || !mqtt_topic) return -1;
    if (mqtt_begin_op(br) != 0) return -1;
    int rc = mqtt_remove_mapping_impl(br, mqtt_topic);
    mqtt_end_op(br);
    return rc;
}

size_t cmq_mqtt_mapping_count(cmq_mqtt_bridge_t *br) {
    if (!br) return 0;
    if (mqtt_begin_op(br) != 0) return 0;
    size_t c = mqtt_mapping_count_impl(br);
    mqtt_end_op(br);
    return c;
}

int cmq_mqtt_find_mapping(cmq_mqtt_bridge_t *br, const char *mqtt_topic,
                           cmq_mqtt_mapping_t *out) {
    if (!br || !mqtt_topic || !out) return -1;
    if (mqtt_begin_op(br) != 0) return -1;
    int rc = mqtt_find_mapping_impl(br, mqtt_topic, out);
    mqtt_end_op(br);
    return rc;
}

const char *cmq_mqtt_topic_to_subject(const char *mqtt_topic, char *buf, size_t len) {
    if (!mqtt_topic || !buf || len == 0) return NULL;
    size_t i = 0;
    for (; i < len - 1 && mqtt_topic[i]; i++) {
        buf[i] = (mqtt_topic[i] == '/') ? '.' : mqtt_topic[i];
    }
    buf[i] = '\0';
    return buf;
}

const char *cmq_mqtt_subject_to_topic(const char *subject, char *buf, size_t len) {
    if (!subject || !buf || len == 0) return NULL;
    size_t i = 0;
    for (; i < len - 1 && subject[i]; i++) {
        buf[i] = (subject[i] == '.') ? '/' : subject[i];
    }
    buf[i] = '\0';
    return buf;
}

static int encode_remaining_length(uint8_t *buf, size_t offset, size_t len_size, uint32_t value) {
    size_t needed = 0;
    uint32_t v = value;
    do {
        needed++;
        v >>= 7;
    } while (v > 0);
    if (needed > 4 || offset + needed > len_size) return -1;

    size_t i = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value > 0) byte |= 0x80;
        buf[offset + i] = byte;
        i++;
    } while (value > 0);
    return (int)i;
}

int cmq_mqtt_encode_connect(uint8_t *buf, size_t len, const char *client_id,
                             int keepalive, int clean_session) {
    if (!buf || len < 20 || !client_id) return -1;
    size_t id_len = strlen(client_id);
    if (id_len > 0xFFFFu || id_len > SIZE_MAX - 10) return -1;
    size_t var_len = 10 + id_len;
    if (var_len > 0x0FFFFFFFu || len < 5 || var_len > len - 5) return -1;

    buf[0] = CMQ_MQTT_CONNECT;
    int rl = encode_remaining_length(buf, 1, len, (uint32_t)var_len);
    if (rl < 0) return -1;
    size_t pos = (size_t)(1 + rl);
    if (pos > len || len - pos < 10 + id_len) return -1;

    buf[pos++] = 0x00; buf[pos++] = 0x04;
    buf[pos++] = 'M';  buf[pos++] = 'Q';
    buf[pos++] = 'T';  buf[pos++] = 'T';
    buf[pos++] = 0x04;
    buf[pos++] = (uint8_t)((clean_session ? 0x02 : 0x00));
    buf[pos++] = (uint8_t)((keepalive >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(keepalive & 0xFF);
    buf[pos++] = (uint8_t)((id_len >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(id_len & 0xFF);
    memcpy(&buf[pos], client_id, id_len);
    return (int)(pos + id_len);
}

int cmq_mqtt_encode_publish(uint8_t *buf, size_t len, const char *topic,
                             const uint8_t *payload, size_t payload_len, int qos,
                             uint16_t packet_id) {
    if (!buf || !topic) return -1;
    if (qos < 0 || qos > 2) return -1;
    if (qos > 0 && packet_id == 0) return -1;
    if (payload_len > 0 && !payload) return -1;
    size_t topic_len = strlen(topic);
    if (topic_len > 0xFFFFu || topic_len > SIZE_MAX - 2) return -1;
    size_t var_len = 2 + topic_len;
    if (payload_len > SIZE_MAX - var_len) return -1;
    var_len += payload_len;
    if (qos > 0) {
        if (var_len > SIZE_MAX - 2) return -1;
        var_len += 2;
    }
    if (var_len > 0x0FFFFFFFu || len < 5 || var_len > len - 5) return -1;

    buf[0] = CMQ_MQTT_PUBLISH | (uint8_t)((qos & 0x03) << 1);
    int rl = encode_remaining_length(buf, 1, len, (uint32_t)var_len);
    if (rl < 0) return -1;
    size_t pos = (size_t)(1 + rl);
    if (pos > len || len - pos < var_len) return -1;

    buf[pos++] = (uint8_t)((topic_len >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(&buf[pos], topic, topic_len);
    pos += topic_len;

    if (qos > 0) {
        buf[pos++] = (uint8_t)(packet_id >> 8);
        buf[pos++] = (uint8_t)(packet_id & 0xFF);
    }

    if (payload_len > 0)
        memcpy(&buf[pos], payload, payload_len);
    return (int)(pos + payload_len);
}

int cmq_mqtt_encode_subscribe(uint8_t *buf, size_t len, const char *topic,
                               int qos, uint16_t packet_id) {
    if (!buf || !topic) return -1;
    if (qos < 0 || qos > 2) return -1;
    if (packet_id == 0) return -1;
    size_t topic_len = strlen(topic);
    if (topic_len > 0xFFFFu || topic_len > SIZE_MAX - 5) return -1;
    size_t var_len = 2 + 2 + topic_len + 1;
    if (var_len > 0x0FFFFFFFu || len < 5 || var_len > len - 5) return -1;

    buf[0] = CMQ_MQTT_SUBSCRIBE | 0x02;
    int rl = encode_remaining_length(buf, 1, len, (uint32_t)var_len);
    if (rl < 0) return -1;
    size_t pos = (size_t)(1 + rl);
    if (pos > len || len - pos < var_len) return -1;

    buf[pos++] = (uint8_t)(packet_id >> 8);
    buf[pos++] = (uint8_t)(packet_id & 0xFF);
    buf[pos++] = (uint8_t)((topic_len >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(&buf[pos], topic, topic_len);
    pos += topic_len;
    buf[pos++] = (uint8_t)(qos & 0x03);
    return (int)pos;
}

int cmq_mqtt_encode_pingreq(uint8_t *buf, size_t len) {
    if (!buf || len < 2) return -1;
    buf[0] = CMQ_MQTT_PINGREQ;
    buf[1] = 0x00;
    return 2;
}

int cmq_mqtt_decode_connack(const uint8_t *buf, size_t len) {
    if (!buf || len < 4) return -1;
    if ((buf[0] & 0xF0) != CMQ_MQTT_CONNACK) return -1;
    /* MQTT 3.1.1 CONNACK: fixed header + remaining length 2. */
    if (buf[1] != 0x02) return -1;
    return buf[3];
}

int cmq_mqtt_decode_packet_type(const uint8_t *buf, size_t len) {
    if (!buf || len < 1) return -1;
    return (buf[0] >> 4) & 0x0F;
}
