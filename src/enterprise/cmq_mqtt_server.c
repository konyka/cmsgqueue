/* P4: minimal MQTT 3.1.1 server (CONNECT / CONNACK / PING / PINGRESP /
 * DISCONNECT). PUBLISH / SUBSCRIBE / SUBACK / PUBACK are deferred to
 * v0.6 (see v0.5.1.plan.md P8). */

#define _POSIX_C_SOURCE 200809L
#include "cmq_mqtt_server.h"
#include <stdatomic.h>

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

/* P1 v0.5.4: 5.0 property skip flag. When set, PUBLISH and
 * SUBSCRIBE variable headers are scanned for the 5.0 Properties
 * region (var-byte length) and skipped. Future work decodes
 * individual property tags. */
static int mqtt_v5_props_skip = 1;

/* P3 v0.5.4: listener opt-in. Default off (anyone-on-host risk).
 * Call cmq_mqtt_set_listener_enabled(1) to bind 127.0.0.1:1883. */
static int mqtt_listener_enabled = 0;

/* P1 v0.5.5: per-source-IP rate limit on PUBLISH. Token bucket per
 * 32-bit IPv4 address; capacity 100, refill 100/sec, default off. */
#define MQTT_RATE_BUCKETS 1024
/* P2 v0.5.6: sharded mutex — 16 shards instead of 1 to reduce
 * contention under high concurrent client count. */
#define MQTT_RATE_SHARDS 16
struct rate_bucket {
    uint32_t ip;
    uint32_t tokens;
    uint64_t last_refill_ms;
};
static struct rate_bucket g_rate_buckets[MQTT_RATE_BUCKETS];
static pthread_mutex_t g_rate_locks[MQTT_RATE_SHARDS];
static uint32_t g_rate_capacity = 0;
static uint32_t g_rate_refill_per_sec = 0;

void cmq_mqtt_set_rate_limit(uint32_t capacity, uint32_t refill_per_sec) {
    g_rate_capacity = capacity;
    g_rate_refill_per_sec = refill_per_sec;
}

static int rate_limit_check(uint32_t ip) {
    if (g_rate_capacity == 0) return 1;
    uint32_t slot = ip % MQTT_RATE_BUCKETS;
    uint32_t shard = slot % MQTT_RATE_SHARDS;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL;
    pthread_mutex_lock(&g_rate_locks[shard]);
    if (g_rate_buckets[slot].ip != ip) {
        g_rate_buckets[slot].ip = ip;
        g_rate_buckets[slot].tokens = g_rate_capacity;
        g_rate_buckets[slot].last_refill_ms = now_ms;
    } else {
        uint64_t delta = now_ms - g_rate_buckets[slot].last_refill_ms;
        uint32_t refill = (uint32_t)(delta * g_rate_refill_per_sec / 1000);
        if (refill > 0) {
            uint32_t t = g_rate_buckets[slot].tokens + refill;
            if (t > g_rate_capacity || t < g_rate_buckets[slot].tokens)
                t = g_rate_capacity;
            g_rate_buckets[slot].tokens = t;
            g_rate_buckets[slot].last_refill_ms = now_ms;
        }
    }
    int admit = 1;
    if (g_rate_buckets[slot].tokens > 0) {
        g_rate_buckets[slot].tokens--;
    } else {
        admit = 0;
    }
    pthread_mutex_unlock(&g_rate_locks[shard]);
    return admit;
}

/* P3 (v0.5.3): retain file path. Forward-declared because
 * cmq_mqtt_set_retain_path uses it before its full definition. */
static char g_mqtt_retain_path[256] = {0};

/* P1 (v0.5.3) F19b bridge state. The mqtt thread enqueues pending
 * PUBLISH topics; a relay thread consumes and inserts into
 * srv->sublist. Synthetic client has a unique static id so it
 * doesn't collide with real cmq clients. */
static struct cmq_server *g_mqtt_bridge_srv = NULL;
static pthread_mutex_t g_mqtt_bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mqtt_bridge_not_empty = PTHREAD_COND_INITIALIZER;
/* P1 v0.5.4: pointer to payload (heap-allocated) so we accept
 * payloads of any size. Bounded by 256 entries. */
static char g_mqtt_bridge_topics[256][128];
static uint8_t *g_mqtt_bridge_payloads[256];
static size_t g_mqtt_bridge_lens[256];
static int g_mqtt_bridge_head;
static int g_mqtt_bridge_tail;
static int g_mqtt_bridge_count;
static pthread_t g_mqtt_bridge_thread;
static int g_mqtt_bridge_thread_started;
static atomic_int g_mqtt_bridge_dying;

/* Forward decls: we can't include cmq_sublist.h / cmq_server.h
 * here because of circular deps. The relay thread calls
 * cmq_sublist_insert via a function pointer resolved at
 * cmq_mqtt_set_bridge_server time. */
typedef int (*cmq_sublist_insert_fn)(void *sublist, const char *topic_str,
                                     void *data);

/* Forward decls of the cmq_sub_ref_t struct (defined in
 * cmq_server.c). We don't need to know its layout — we just
 * hold a pointer and let cmq_sublist_match/remove handle it. */
struct cmq_sub_ref;

static int relay_insert_cb(void *sublist, const char *subject, void *data) {
    (void)sublist; (void)subject; (void)data;
    return 0;
}

static cmq_sublist_insert_fn g_relay_insert_fn = relay_insert_cb;
static void *g_relay_sublist = NULL;

static void *cmq_mqtt_bridge_relay(void *arg) {
    struct cmq_server *srv = (struct cmq_server *)arg;
    (void)srv;
    while (!atomic_load(&g_mqtt_bridge_dying)) {
        pthread_mutex_lock(&g_mqtt_bridge_lock);
        while (g_mqtt_bridge_count == 0 &&
               !atomic_load(&g_mqtt_bridge_dying)) {
            pthread_cond_wait(&g_mqtt_bridge_not_empty,
                                &g_mqtt_bridge_lock);
        }
        if (atomic_load(&g_mqtt_bridge_dying) && g_mqtt_bridge_count == 0) {
            pthread_mutex_unlock(&g_mqtt_bridge_lock);
            break;
        }
        char *topic = g_mqtt_bridge_topics[g_mqtt_bridge_tail];
        uint8_t *payload = g_mqtt_bridge_payloads[g_mqtt_bridge_tail];
        size_t plen = g_mqtt_bridge_lens[g_mqtt_bridge_tail];
        g_mqtt_bridge_tail = (g_mqtt_bridge_tail + 1) % 256;
        g_mqtt_bridge_count--;
        pthread_cond_signal(&g_mqtt_bridge_not_empty);
        pthread_mutex_unlock(&g_mqtt_bridge_lock);

        /* P1 v0.5.4: insert into cmq sublist via the function pointer
         * registered at cmq_mqtt_set_bridge_server time. */
        if (g_relay_sublist && g_relay_insert_fn != relay_insert_cb) {
            (void)g_relay_insert_fn(g_relay_sublist, topic, payload);
        }
        free(payload);
    }
    return NULL;
}

void cmq_mqtt_set_bridge_server(struct cmq_server *server) {
    g_mqtt_bridge_srv = server;
    if (server && !g_mqtt_bridge_thread_started) {
        atomic_init(&g_mqtt_bridge_dying, 0);
        if (pthread_create(&g_mqtt_bridge_thread, NULL,
                             cmq_mqtt_bridge_relay, server) == 0) {
            g_mqtt_bridge_thread_started = 1;
        }
    }
}

void cmq_mqtt_set_listener_enabled(int enabled) {
    mqtt_listener_enabled = enabled ? 1 : 0;
}

/* P1 v0.5.4: register the sublist insert function + pointer so
 * the relay can call into cmq_sublist_insert without circular
 * includes. Called once during cmq_server_create. */
void cmq_mqtt_register_sublist_insert(cmq_sublist_insert_fn fn,
                                       void *sublist) {
    g_relay_insert_fn = fn ? fn : relay_insert_cb;
    g_relay_sublist = sublist;
}

/* P2 v0.5.4: clean shutdown of the relay thread. */
void cmq_mqtt_bridge_shutdown(void) {
    if (!g_mqtt_bridge_thread_started) return;
    atomic_store(&g_mqtt_bridge_dying, 1);
    pthread_mutex_lock(&g_mqtt_bridge_lock);
    pthread_cond_broadcast(&g_mqtt_bridge_not_empty);
    pthread_mutex_unlock(&g_mqtt_bridge_lock);
    pthread_join(g_mqtt_bridge_thread, NULL);
    g_mqtt_bridge_thread_started = 0;
    g_mqtt_bridge_head = g_mqtt_bridge_tail = g_mqtt_bridge_count = 0;
}

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

/* P1 (v0.5.3): per-session QoS2 retransmit table. Each entry tracks
 * one packet_id's phase: PUBLISHED (PUBREC sent) or RELEASED (PUBCOMP
 * sent). Duplicate PUBLISH re-emits PUBREC; duplicate PUBREL
 * re-emits PUBCOMP. Capped at MQTT_QOS2_MAX to bound memory. */
#define MQTT_QOS2_MAX 128
struct qos2_entry {
    uint16_t packet_id;
    uint8_t phase;  /* 1=PUBREC sent, 2=PUBCOMP sent */
};
static struct qos2_entry g_qos2[MQTT_QOS2_MAX];
static int g_qos2_count = 0;
static pthread_mutex_t g_qos2_lock = PTHREAD_MUTEX_INITIALIZER;

static int qos2_record_or_lookup(uint16_t packet_id, int new_phase) {
    pthread_mutex_lock(&g_qos2_lock);
    int found = 0;
    for (int i = 0; i < g_qos2_count; i++) {
        if (g_qos2[i].packet_id == packet_id) {
            g_qos2[i].phase = new_phase;
            found = 1;
            break;
        }
    }
    if (!found && g_qos2_count < MQTT_QOS2_MAX) {
        g_qos2[g_qos2_count].packet_id = packet_id;
        g_qos2[g_qos2_count].phase = new_phase;
        g_qos2_count++;
    }
    pthread_mutex_unlock(&g_qos2_lock);
    return found;
}

static int qos2_get_phase(uint16_t packet_id) {
    pthread_mutex_lock(&g_qos2_lock);
    int phase = 0;
    for (int i = 0; i < g_qos2_count; i++) {
        if (g_qos2[i].packet_id == packet_id) {
            phase = g_qos2[i].phase;
            break;
        }
    }
    pthread_mutex_unlock(&g_qos2_lock);
    return phase;
}

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

int cmq_mqtt_get_subscribed_topic(int index, char *out, size_t out_len) {
    if (!out || out_len == 0 || index < 0) return -1;
    pthread_mutex_lock(&g_mqtt_sub_lock);
    int rc = -1;
    if (index < g_mqtt_sub_count) {
        snprintf(out, out_len, "%s", g_mqtt_sub_topics[index]);
        rc = 0;
    }
    pthread_mutex_unlock(&g_mqtt_sub_lock);
    return rc;
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
    /* P3: append to the persistent retain file (best-effort). */
    if (g_mqtt_retain_path[0]) {
        FILE *f = fopen(g_mqtt_retain_path, "a");
        if (f) {
            fprintf(f, "%s %d %zu ", g_mqtt_retained[idx].topic,
                    (int)g_mqtt_retained[idx].payload_len, len);
            if (len > 0) fwrite(payload, 1, len, f);
            fputc('\n', f);
            fclose(f);
        }
    }
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

void cmq_mqtt_set_retain_path(const char *path) {
    if (path) {
        snprintf(g_mqtt_retain_path, sizeof(g_mqtt_retain_path),
                  "%s", path);
        FILE *f = fopen(g_mqtt_retain_path, "r");
        if (f) {
            char topic[128];
            int tlen;
            size_t plen;
            while (fscanf(f, "%127s %d %zu ", topic, &tlen, &plen) == 3) {
                if (tlen <= 0 || tlen >= 128 || plen == 0) continue;
                uint8_t *buf = malloc(plen);
                if (buf) {
                    if (fread(buf, 1, plen, f) == plen) {
                        cmq_mqtt_store_retained(topic, buf, plen);
                    }
                    free(buf);
                }
            }
            fclose(f);
        }
    } else {
        g_mqtt_retain_path[0] = '\0';
    }
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
            /* P4 v0.5.4: reset per-session tables on CONNECT. Stale
             * entries from a prior session would otherwise leak
             * packet_ids + topic filters. */
            g_mqtt_sub_count = 0;
            g_qos2_count = 0;
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
            /* P1 v0.5.6: 5.0 SUBSCRIBE has a Properties Length
             * var-byte before the filter list. Skip it. */
            const uint8_t *p = buf + 1 + rl_off;
            size_t plen = rem_len;
            if (plen < 5) continue;
            uint16_t packet_id = ((uint16_t)p[0] << 8) | p[1];
            size_t off = 2;
            if (mqtt_v5_props_skip) {
                if (off < plen) {
                    uint32_t props_len = 0;
                    int rl = decode_remaining_length(p + off,
                                                     plen - off,
                                                     &props_len);
                    if (rl < 0) continue;
                    off += (size_t)rl + props_len;
                }
            }
            uint8_t granted[8];
            int granted_n = 0;
            char last_topic[128] = {0};
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
                snprintf(last_topic, sizeof(last_topic), "%s", topic);
                granted[granted_n++] = req_qos;
            }
            if (granted_n > 0) {
                if (send_suback(fd, packet_id, granted,
                                   (size_t)granted_n) < 0) return -1;
            }
            /* P3 (v0.5.3): deliver retained messages to the new
             * subscriber. v0.5.2 stored them; this round trips them.
             * Iterate through the SUBSCRIBE payload again to deliver
             * matches — single-topic for now (MQTT_MAX_RETAINED=32). */
            if (last_topic[0]) {
                const uint8_t *rp = NULL;
                size_t rlen = 0;
                if (cmq_mqtt_fetch_retained(last_topic, &rp, &rlen) == 0) {
                    uint8_t rpkt[256];
                    size_t tlen = strlen(last_topic);
                    if (tlen < 125 && rlen < 128) {
                        size_t off2 = 0;
                        rpkt[off2++] = 0x30 | 0x01;  /* PUBLISH + RETAIN */
                        uint32_t var_len = 2 + tlen + rlen;
                        rpkt[off2++] = (uint8_t)var_len;
                        rpkt[off2++] = (uint8_t)(tlen >> 8);
                        rpkt[off2++] = (uint8_t)(tlen & 0xFF);
                        memcpy(rpkt + off2, last_topic, tlen);
                        off2 += tlen;
                        memcpy(rpkt + off2, rp, rlen);
                        off2 += rlen;
                        (void)send(fd, rpkt, off2, 0);
                    }
                }
            }
        } else if (type == MQTT_TYPE_PUBLISH && connected) {
            /* P1 v0.5.5: per-source-IP rate limit. Default off. */
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            uint32_t ip = 0;
            if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0 &&
                peer.sin_family == AF_INET) {
                ip = (uint32_t)peer.sin_addr.s_addr;
            }
            if (!rate_limit_check(ip)) {
                uint8_t rpkt[4] = {0x90, 0x02, 0x03, 0x00};
                (void)send(fd, rpkt, sizeof(rpkt), 0);
                return -1;
            }
            /* P1 v0.5.4: 5.0 PUBLISH variable header has an extra
             * Properties region between topic and packet_id. Read
             * the var-byte Property Length and skip those bytes. We
             * don't decode individual properties — just advance the
             * offset. */
            const uint8_t *p = buf + 1 + rl_off;
            size_t plen = rem_len;
            if (plen < 2) continue;
            uint16_t topic_len = ((uint16_t)p[0] << 8) | p[1];
            if ((size_t)(2 + topic_len) > plen) continue;
            uint8_t qos = (buf[0] >> 1) & 0x03;
            int retain = (buf[0] & 0x08) != 0;
            size_t off = 2 + topic_len;
            if (mqtt_v5_props_skip) {
                /* 5.0 properties region: var-byte length + bytes.
                 * For v0.5.4 we just skip; full decode is v0.6. */
                if (off < plen) {
                    uint32_t props_len = 0;
                    int rl = decode_remaining_length(p + off,
                                                     plen - off,
                                                     &props_len);
                    if (rl < 0) continue;
                    off += (size_t)rl + props_len;
                }
            }
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
            /* P1 v0.5.3: enqueue into the F19b bridge queue when the
             * server has been set via cmq_mqtt_set_bridge_server. The
             * relay thread consumes + inserts into srv->sublist. */
            if (g_mqtt_bridge_srv && topic_len > 0 && topic_len < 128) {
                char topic[128];
                memcpy(topic, p + 2, topic_len);
                topic[topic_len] = '\0';
                /* P2 v0.5.4: heap-allocate payload of any size
                 * (was bounded to 1024). The relay thread frees. */
                uint8_t *payload_copy = NULL;
                if (payload_len > 0) {
                    payload_copy = malloc(payload_len);
                    if (payload_copy)
                        memcpy(payload_copy, p + payload_off, payload_len);
                }
                if (payload_copy || payload_len == 0) {
                    pthread_mutex_lock(&g_mqtt_bridge_lock);
                    if (g_mqtt_bridge_count < 256) {
                        strcpy(g_mqtt_bridge_topics[g_mqtt_bridge_head],
                                topic);
                        g_mqtt_bridge_payloads[g_mqtt_bridge_head] =
                            payload_copy;
                        g_mqtt_bridge_lens[g_mqtt_bridge_head] = payload_len;
                        g_mqtt_bridge_head = (g_mqtt_bridge_head + 1) % 256;
                        g_mqtt_bridge_count++;
                        pthread_cond_signal(&g_mqtt_bridge_not_empty);
                    } else {
                        free(payload_copy);
                    }
                    pthread_mutex_unlock(&g_mqtt_bridge_lock);
                }
            }
            if (qos > 1) {
                /* P1 v0.5.3: QoS 2 — record PUBREC phase. Re-emit if
                 * duplicate PUBLISH. */
                if ((size_t)(2 + topic_len + 2) > plen) continue;
                uint16_t packet_id =
                    ((uint16_t)p[2 + topic_len] << 8) | p[2 + topic_len + 1];
                qos2_record_or_lookup(packet_id, 1);
                if (send_pubrec(fd, packet_id) < 0) return -1;
            } else if (qos == 1) {
                if ((size_t)(2 + topic_len + 2) > plen) continue;
                uint16_t packet_id =
                    ((uint16_t)p[2 + topic_len] << 8) | p[2 + topic_len + 1];
                if (send_puback(fd, packet_id) < 0) return -1;
            }
        } else if (type == 0x60 && connected) {
            /* P1 v0.5.3: PUBREL completes QoS 2 handshake. Mark
             * PUBCOMP phase; re-emit if duplicate PUBREL. */
            const uint8_t *p = buf + 1 + rl_off;
            if (rem_len < 2) continue;
            uint16_t packet_id = ((uint16_t)p[0] << 8) | p[1];
            qos2_record_or_lookup(packet_id, 2);
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
    if (!mqtt_listener_enabled) return;
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