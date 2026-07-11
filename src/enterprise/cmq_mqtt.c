#define _POSIX_C_SOURCE 200809L
#include "cmq_mqtt.h"
#include "cmq_route.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

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
};

cmq_mqtt_bridge_t *cmq_mqtt_bridge_create(const char *client_id) {
    if (!client_id) return NULL;
    cmq_mqtt_bridge_t *br = calloc(1, sizeof(cmq_mqtt_bridge_t));
    if (!br) return NULL;
    strncpy(br->client_id, client_id, CMQ_MQTT_CLIENT_ID - 1);
    br->fd = -1;
    br->keepalive_ms = 60000;
    br->clean_session = 1;
    return br;
}

void cmq_mqtt_bridge_destroy(cmq_mqtt_bridge_t *br) {
    if (!br) return;
    if (br->fd >= 0) close(br->fd);
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
    if (br->connected) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_MQTT_CONNECT_MS) != 0) {
        close(fd);
        return -1;
    }
    mqtt_set_nonblock(fd);

    uint8_t cbuf[256];
    int keepalive_s = br->keepalive_ms > 0 ? br->keepalive_ms / 1000 : 60;
    if (keepalive_s < 1) keepalive_s = 1;
    int clen = cmq_mqtt_encode_connect(cbuf, sizeof(cbuf), br->client_id,
                                        keepalive_s, br->clean_session);
    if (clen < 0 || mqtt_write_all(fd, cbuf, (size_t)clen) != 0 ||
        mqtt_read_connack(fd) != 0) {
        close(fd);
        return -1;
    }

    br->fd = fd;
    br->connected = 1;
    strncpy(br->addr, addr, sizeof(br->addr) - 1);
    br->addr[sizeof(br->addr) - 1] = '\0';
    br->port = port;
    return 0;
}

int cmq_mqtt_bridge_disconnect(cmq_mqtt_bridge_t *br) {
    if (!br) return -1;
    if (br->fd >= 0) close(br->fd);
    br->fd = -1;
    br->connected = 0;
    return 0;
}

int cmq_mqtt_bridge_is_connected(cmq_mqtt_bridge_t *br) {
    return br ? br->connected : 0;
}

const char *cmq_mqtt_client_id(cmq_mqtt_bridge_t *br) {
    return br ? br->client_id : NULL;
}

cmq_mqtt_bridge_info_t cmq_mqtt_bridge_info(cmq_mqtt_bridge_t *br) {
    cmq_mqtt_bridge_info_t info = {0};
    if (br) {
        strncpy(info.client_id, br->client_id, CMQ_MQTT_CLIENT_ID - 1);
        strncpy(info.addr, br->addr, sizeof(info.addr) - 1);
        info.port = br->port;
        info.keepalive_ms = br->keepalive_ms;
        info.clean_session = br->clean_session;
        info.connected = br->connected;
        info.messages_in = br->messages_in;
        info.messages_out = br->messages_out;
    }
    return info;
}

int cmq_mqtt_add_mapping(cmq_mqtt_bridge_t *br, const char *cmq_subject,
                          const char *mqtt_topic, int qos) {
    if (!br || !cmq_subject || !mqtt_topic) return -1;
    if (br->mapping_count >= CMQ_MQTT_MAX_MAPPINGS) return -1;
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0) {
            strncpy(br->mappings[i].cmq_subject, cmq_subject,
                    CMQ_MQTT_TOPIC_MAX - 1);
            br->mappings[i].qos = qos;
            return 0;
        }
    }
    cmq_mqtt_mapping_t *m = &br->mappings[br->mapping_count++];
    strncpy(m->cmq_subject, cmq_subject, CMQ_MQTT_TOPIC_MAX - 1);
    strncpy(m->mqtt_topic, mqtt_topic, CMQ_MQTT_TOPIC_MAX - 1);
    m->qos = qos;
    m->active = 1;
    return 0;
}

int cmq_mqtt_remove_mapping(cmq_mqtt_bridge_t *br, const char *mqtt_topic) {
    if (!br || !mqtt_topic) return -1;
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0) {
            memmove(&br->mappings[i], &br->mappings[i + 1],
                    (br->mapping_count - i - 1) * sizeof(cmq_mqtt_mapping_t));
            br->mapping_count--;
            return 0;
        }
    }
    return -1;
}

size_t cmq_mqtt_mapping_count(cmq_mqtt_bridge_t *br) {
    return br ? br->mapping_count : 0;
}

cmq_mqtt_mapping_t *cmq_mqtt_find_mapping(cmq_mqtt_bridge_t *br,
                                            const char *mqtt_topic) {
    if (!br || !mqtt_topic) return NULL;
    for (size_t i = 0; i < br->mapping_count; i++) {
        if (strcmp(br->mappings[i].mqtt_topic, mqtt_topic) == 0)
            return &br->mappings[i];
    }
    return NULL;
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
                             const uint8_t *payload, size_t payload_len, int qos) {
    if (!buf || !topic) return -1;
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
        buf[pos++] = 0x00;
        buf[pos++] = 0x01;
    }

    if (payload_len > 0)
        memcpy(&buf[pos], payload, payload_len);
    return (int)(pos + payload_len);
}

int cmq_mqtt_encode_subscribe(uint8_t *buf, size_t len, const char *topic, int qos) {
    if (!buf || !topic) return -1;
    size_t topic_len = strlen(topic);
    if (topic_len > 0xFFFFu || topic_len > SIZE_MAX - 5) return -1;
    size_t var_len = 2 + 2 + topic_len + 1;
    if (var_len > 0x0FFFFFFFu || len < 5 || var_len > len - 5) return -1;

    buf[0] = CMQ_MQTT_SUBSCRIBE | 0x02;
    int rl = encode_remaining_length(buf, 1, len, (uint32_t)var_len);
    if (rl < 0) return -1;
    size_t pos = (size_t)(1 + rl);
    if (pos > len || len - pos < var_len) return -1;

    buf[pos++] = 0x00; buf[pos++] = 0x01;
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
    return buf[3];
}

int cmq_mqtt_decode_packet_type(const uint8_t *buf, size_t len) {
    if (!buf || len < 1) return -1;
    return (buf[0] >> 4) & 0x0F;
}
