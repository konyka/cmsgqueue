/* P4: minimal MQTT 3.1.1 server (CONNECT / CONNACK / PING / PINGRESP /
 * DISCONNECT). PUBLISH / SUBSCRIBE / SUBACK / PUBACK are deferred to
 * v0.6 (see v0.5.1.plan.md P8). */

#define _POSIX_C_SOURCE 200809L
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MQTT_LISTEN_PORT 1883
#define MQTT_MAX_PACKET  65536
#define MQTT_TYPE_CONNECT     0x10
#define MQTT_TYPE_CONNACK     0x20
#define MQTT_TYPE_PUBLISH     0x30
#define MQTT_TYPE_PUBACK      0x40
#define MQTT_TYPE_SUBSCRIBE   0x80
#define MQTT_TYPE_SUBACK      0x90
#define MQTT_TYPE_PINGREQ     0xC0
#define MQTT_TYPE_PINGRESP    0xD0
#define MQTT_TYPE_DISCONNECT  0xE0
#define MQTT_TYPE_MASK        0xF0

#define MQTT_CONNACK_ACCEPTED  0x00
#define MQTT_CONNACK_PROTO     0x01
#define MQTT_CONNACK_ID        0x02
#define MQTT_CONNACK_UNAVAIL   0x03
#define MQTT_CONNACK_BAD_AUTH  0x04

static int encode_remaining_length(uint8_t *buf, size_t len) {
    int n = 0;
    do {
        uint8_t b = (uint8_t)(len & 0x7F);
        len >>= 7;
        if (len > 0) b |= 0x80;
        buf[n++] = b;
    } while (len > 0);
    return n;
}

static int decode_remaining_length(const uint8_t *buf, size_t buf_len,
                                    uint32_t *out_len) {
    uint32_t multiplier = 1;
    uint32_t value = 0;
    int n = 0;
    do {
        if (n >= buf_len || n >= 4) return -1;
        uint8_t b = buf[n++];
        value += (uint32_t)(b & 0x7F) * multiplier;
        if ((b & 0x80) == 0) break;
        multiplier *= 128;
    } while (1);
    *out_len = value;
    return n;
}

static int send_connack(int fd, uint8_t return_code) {
    uint8_t pkt[4];
    pkt[0] = MQTT_TYPE_CONNACK;
    pkt[1] = 0x02;
    pkt[2] = 0x00;
    pkt[3] = return_code;
    return (int)send(fd, pkt, sizeof(pkt), 0);
}

static int send_pingresp(int fd) {
    uint8_t pkt[2] = { MQTT_TYPE_PINGRESP, 0x00 };
    return (int)send(fd, pkt, sizeof(pkt), 0);
}

/* P8: PUBACK for QoS 1 PUBLISH. packet_id (2B) only. */
static int send_puback(int fd, uint16_t packet_id) {
    uint8_t pkt[4];
    pkt[0] = MQTT_TYPE_PUBACK;
    pkt[1] = 0x02;
    pkt[2] = (uint8_t)(packet_id >> 8);
    pkt[3] = (uint8_t)(packet_id & 0xFF);
    return (int)send(fd, pkt, sizeof(pkt), 0);
}

static int send_pubrec(int fd, uint16_t packet_id) {
    uint8_t pkt[4];
    pkt[0] = MQTT_TYPE_PUBACK;  /* PUBREC shares the type nibble 0x5 */
    pkt[0] = 0x50;
    pkt[1] = 0x02;
    pkt[2] = (uint8_t)(packet_id >> 8);
    pkt[3] = (uint8_t)(packet_id & 0xFF);
    return (int)send(fd, pkt, sizeof(pkt), 0);
}

static int send_pubcomp(int fd, uint16_t packet_id) {
    uint8_t pkt[4];
    pkt[0] = 0x70;
    pkt[1] = 0x02;
    pkt[2] = (uint8_t)(packet_id >> 8);
    pkt[3] = (uint8_t)(packet_id & 0xFF);
    return (int)send(fd, pkt, sizeof(pkt), 0);
}

/* P1: SUBACK for SUBSCRIBE. payload = sequence of granted QoS bytes
 * (here we always grant QoS 0). packet_id (2B) + at least 1 byte. */
static int send_suback(int fd, uint16_t packet_id,
                        const uint8_t *granted_qos, size_t n) {
    uint8_t buf[16];
    if (n > 14) return -1;
    buf[0] = MQTT_TYPE_SUBACK;
    buf[1] = (uint8_t)(2 + n);
    buf[2] = (uint8_t)(packet_id >> 8);
    buf[3] = (uint8_t)(packet_id & 0xFF);
    memcpy(buf + 4, granted_qos, n);
    return (int)send(fd, buf, 4 + n, 0);
}

#define MQTT_MAX_SUBS 64
static char g_mqtt_sub_topics[MQTT_MAX_SUBS][128];
static int g_mqtt_sub_count = 0;
static pthread_mutex_t g_mqtt_sub_lock = PTHREAD_MUTEX_INITIALIZER;

int cmq_mqtt_record_subscriber(const char *topic_filter) {
    if (!topic_filter || !*topic_filter) return -1;
    pthread_mutex_lock(&g_mqtt_sub_lock);
    int rc = -1;
    if (g_mqtt_sub_count < MQTT_MAX_SUBS) {
        snprintf(g_mqtt_sub_topics[g_mqtt_sub_count], 128, "%s",
                  topic_filter);
        g_mqtt_sub_count++;
        rc = 0;
    }
    pthread_mutex_unlock(&g_mqtt_sub_lock);
    return rc;
}

int cmq_mqtt_subscriber_count(void) {
    pthread_mutex_lock(&g_mqtt_sub_lock);
    int n = g_mqtt_sub_count;
    pthread_mutex_unlock(&g_mqtt_sub_lock);
    return n;
}

/* P4 (v0.5.2): retained-message store. Last retained payload per
 * topic, delivered to new SUBSCRIBE on that topic. */
#define MQTT_MAX_RETAINED 32
struct retained_entry {
    char topic[128];
    uint8_t *payload;
    size_t payload_len;
};
static struct retained_entry g_mqtt_retained[MQTT_MAX_RETAINED];
static int g_mqtt_retained_count = 0;
static pthread_mutex_t g_mqtt_retained_lock = PTHREAD_MUTEX_INITIALIZER;

void cmq_mqtt_store_retained(const char *topic, const uint8_t *payload,
                              size_t len) {
    if (!topic || !*topic || (!payload && len > 0)) return;
    pthread_mutex_lock(&g_mqtt_retained_lock);
    int idx = -1;
    for (int i = 0; i < g_mqtt_retained_count; i++) {
        if (strcmp(g_mqtt_retained[i].topic, topic) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (g_mqtt_retained_count >= MQTT_MAX_RETAINED) {
            pthread_mutex_unlock(&g_mqtt_retained_lock);
            return;
        }
        idx = g_mqtt_retained_count++;
        snprintf(g_mqtt_retained[idx].topic, sizeof(g_mqtt_retained[idx].topic),
                  "%s", topic);
    } else {
        free(g_mqtt_retained[idx].payload);
    }
    g_mqtt_retained[idx].payload = malloc(len > 0 ? len : 1);
    if (g_mqtt_retained[idx].payload && len > 0) {
        memcpy(g_mqtt_retained[idx].payload, payload, len);
    }
    g_mqtt_retained[idx].payload_len = len;
    pthread_mutex_unlock(&g_mqtt_retained_lock);
}

int cmq_mqtt_fetch_retained(const char *topic, const uint8_t **out,
                             size_t *out_len) {
    if (!topic || !out || !out_len) return -1;
    pthread_mutex_lock(&g_mqtt_retained_lock);
    int rc = -1;
    for (int i = 0; i < g_mqtt_retained_count; i++) {
        if (strcmp(g_mqtt_retained[i].topic, topic) == 0) {
            *out = g_mqtt_retained[i].payload;
            *out_len = g_mqtt_retained[i].payload_len;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_mqtt_retained_lock);
    return rc;
}

/* P1 (v0.5.2): static credentials for the MQTT listener. When
 * non-empty, CONNECT must include matching Username/Password.
 * Compile-time settable via env at startup; default is empty (no
 * auth). Future work: per-server config plumbing. */
static const char *g_mqtt_user = "";
static const char *g_mqtt_pass = "";

void cmq_mqtt_set_credentials(const char *user, const char *pass) {
    if (user) g_mqtt_user = user;
    if (pass) g_mqtt_pass = pass;
}

static int parse_connect(const uint8_t *buf, size_t len,
                          char *client_id_out, size_t client_id_max,
                          uint8_t *flags_out,
                          char *user_out, size_t user_max,
                          char *pass_out, size_t pass_max) {
    if (len < 8) return -1;
    if (buf[0] != 0 || buf[1] != 4) return -1;
    if (memcmp(buf + 2, "MQTT", 4) != 0) return -1;
    uint8_t proto_level = buf[6];
    if (proto_level != 0x04 && proto_level != 0x05) return -1;
    /* P3 (v0.5.2): MQTT 5.0 (proto_level=0x05) is accepted at the
     * CONNECT level; 5.0 properties (variable-length region) are
     * skipped over for now (we don't decode them). */
    int is_v5 = (proto_level == 0x05);
    uint8_t flags = buf[7];
    if (flags_out) *flags_out = flags;
    size_t off = 10;
    if (is_v5) {
        if (off + 1 > len) return -1;
        uint32_t props_len = 0;
        int rl = decode_remaining_length(buf + off, len - off, &props_len);
        if (rl < 0) return -1;
        off += (size_t)rl + props_len;
    }
    if (off + 2 > len) return -1;
    uint16_t cid_len = ((uint16_t)buf[off] << 8) | buf[off + 1];
    off += 2;
    if (cid_len == 0 || off + cid_len > len) return -1;
    if (cid_len >= client_id_max) cid_len = (uint16_t)(client_id_max - 1);
    memcpy(client_id_out, buf + off, cid_len);
    client_id_out[cid_len] = '\0';
    off += cid_len;

    user_out[0] = '\0';
    pass_out[0] = '\0';
    if (flags & 0x04) {
        if (off + 2 > len) return -1;
        uint16_t wlen = ((uint16_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + wlen > len || wlen >= user_max) return -1;
        memcpy(user_out, buf + off, wlen);
        user_out[wlen] = '\0';
        off += wlen;
    }
    if (flags & 0x02) {
        if (off + 2 > len) return -1;
        uint16_t wlen = ((uint16_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + wlen > len || wlen >= pass_max) return -1;
        memcpy(pass_out, buf + off, wlen);
        pass_out[wlen] = '\0';
    }
    return 0;
}

static int mqtt_handle_client(int fd) {
    uint8_t buf[MQTT_MAX_PACKET];
    char client_id[64] = {0};
    char user[64] = {0};
    char pass[64] = {0};
    int connected = 0;

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return -1;
size_t got = (size_t)n;

            if (got < 2) continue;
            uint8_t type = buf[0] & MQTT_TYPE_MASK;
            uint32_t rem_len = 0;
            int rl_off = decode_remaining_length(buf + 1, got - 1, &rem_len);
            if (rl_off < 0) return -1;
            if ((size_t)(rl_off + 1 + rem_len) > got) {
                return -1;
            }

        if (type == MQTT_TYPE_CONNECT && !connected) {
            if (parse_connect(buf + 1 + rl_off, rem_len,
                                client_id, sizeof(client_id), NULL,
                                user, sizeof(user), pass,
                                sizeof(pass)) == 0) {
                int auth_ok = 1;
                if (g_mqtt_user[0] || g_mqtt_pass[0]) {
                    if (strcmp(user, g_mqtt_user) != 0 ||
                        strcmp(pass, g_mqtt_pass) != 0) {
                        auth_ok = 0;
                    }
                }
                if (auth_ok) {
                    if (send_connack(fd, MQTT_CONNACK_ACCEPTED) < 0) return -1;
                    connected = 1;
                } else {
                    send_connack(fd, MQTT_CONNACK_BAD_AUTH);
                    return -1;
                }
            } else {
                send_connack(fd, MQTT_CONNACK_PROTO);
                return -1;
            }
        } else if (type == MQTT_TYPE_PINGREQ && connected) {
            if (send_pingresp(fd) < 0) return -1;
        } else if (type == MQTT_TYPE_DISCONNECT) {
            return 0;
        } else if (type == MQTT_TYPE_SUBSCRIBE && connected) {
            /* P1: SUBSCRIBE variable header = packet_id (2B).
             * Payload = list of (topic_filter_len:2B + topic + qos:1B).
             * We accept, record the topic filter, and grant QoS 0. */
            const uint8_t *p = buf + 1 + rl_off;
            size_t plen = rem_len;
            if (plen < 5) continue;
            uint16_t packet_id = ((uint16_t)p[0] << 8) | p[1];
            size_t off = 2;
            uint8_t granted[8];
            int granted_n = 0;
            while (off + 2 < plen && granted_n < 8) {
                uint16_t tlen = ((uint16_t)p[off] << 8) | p[off + 1];
                off += 2;
                if (off + tlen + 1 > plen) break;
                char topic[128];
                if (tlen >= sizeof(topic)) tlen = sizeof(topic) - 1;
                memcpy(topic, p + off, tlen);
                topic[tlen] = '\0';
                off += tlen;
                uint8_t req_qos = p[off];
                off += 1;
                if (req_qos > 1) req_qos = 1;
                cmq_mqtt_record_subscriber(topic);
                granted[granted_n++] = req_qos;
            }
            if (granted_n > 0) {
                if (send_suback(fd, packet_id, granted,
                                   (size_t)granted_n) < 0) return -1;
            }
        } else if (type == MQTT_TYPE_PUBLISH && connected) {
            /* P8: PUBLISH with QoS 0 (no packet_id) or QoS 1.
             * We accept and emit PUBACK for QoS 1. The variable
             * header is: topic_name (length-prefixed string) +
             * packet_id (2B, only if QoS > 0). We do NOT bridge
             * into the cmq sublist (deferred to v0.6). */
            const uint8_t *p = buf + 1 + rl_off;
            size_t plen = rem_len;
            if (plen < 2) continue;
            uint16_t topic_len = ((uint16_t)p[0] << 8) | p[1];
            if ((size_t)(2 + topic_len) > plen) continue;
            uint8_t qos = (buf[0] >> 1) & 0x03;
            int retain = (buf[0] & 0x08) != 0;
            /* P4: PUBLISH payload starts after topic + packet_id (if QoS>0). */
            size_t payload_off = 2 + topic_len;
            if (qos > 0) payload_off += 2;
            size_t payload_len = 0;
            if (payload_off <= plen) payload_len = plen - payload_off;
            if (retain && topic_len > 0 && topic_len < 128) {
                char topic[128];
                memcpy(topic, p + 2, topic_len);
                topic[topic_len] = '\0';
                cmq_mqtt_store_retained(topic, p + payload_off,
                                          payload_len);
            }
            if (qos > 1) {
                /* P2: QoS 2 — emit PUBREC. We don't track per-packet-id
                 * state; duplicate PUBREL on the same id is accepted. */
                if ((size_t)(2 + topic_len + 2) > plen) continue;
                uint16_t packet_id =
                    ((uint16_t)p[2 + topic_len] << 8) | p[2 + topic_len + 1];
                if (send_pubrec(fd, packet_id) < 0) return -1;
            } else if (qos == 1) {
                if ((size_t)(2 + topic_len + 2) > plen) continue;
                uint16_t packet_id =
                    ((uint16_t)p[2 + topic_len] << 8) | p[2 + topic_len + 1];
                if (send_puback(fd, packet_id) < 0) return -1;
            }
        } else if (type == 0x60 && connected) {
            /* P2: PUBREL for QoS 2 handshake completion. */
            const uint8_t *p = buf + 1 + rl_off;
            if (rem_len < 2) continue;
            uint16_t packet_id = ((uint16_t)p[0] << 8) | p[1];
            if (send_pubcomp(fd, packet_id) < 0) return -1;
        } else if (!connected) {
            send_connack(fd, MQTT_CONNACK_PROTO);
            return -1;
        }
    }
}

static void *mqtt_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    mqtt_handle_client(fd);
    close(fd);
    return NULL;
}

int cmq_mqtt_server_listen(const char *bind_addr, int port) {
    (void)bind_addr;
    (void)port;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MQTT_LISTEN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    close(s);
    return (rc == 0) ? 1 : 0;
}

void cmq_mqtt_server_start_listener(struct cmq_server *server) {
    (void)server;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MQTT_LISTEN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return;
    }
    if (listen(s, 16) != 0) {
        close(s);
        return;
    }
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) break;
        pthread_t t;
        pthread_create(&t, NULL, mqtt_thread, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    close(s);
}