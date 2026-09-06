#ifndef CMQ_MQTT_SERVER_H
#define CMQ_MQTT_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F19: Server-side MQTT 3.1.1 / 5.0 listener.
 *
 * CONNECT/CONNACK, PUBLISH (QoS 0/1/2 handshake), SUBSCRIBE/SUBACK,
 * PINGREQ/PINGRESP, DISCONNECT, retain, topic match, optional
 * bridge into cmq_sublist. MQTT 5.0 property lists are decoded
 * (v0.5.43). Will messages and persistent sessions are still
 * deferred (R6).
 *
 * cmq_mqtt_server_listen probes 127.0.0.1:1883 bind; the real
 * accept loop starts via cmq_mqtt_server_start_listener after
 * cmq_mqtt_set_listener_enabled(1).
 */

#define CMQ_MQTT_USER_PROPS_MAX 4

typedef struct cmq_mqtt_props {
    uint8_t payload_format;   /* 0 or 1; 0xFF = unset */
    uint32_t expiry_interval; /* 0 = unset */
    uint16_t topic_alias;     /* 0 = unset */
    uint32_t sub_id;          /* 0 = unset */
    const uint8_t *content_type;
    uint16_t content_type_len;
    const uint8_t *response_topic;
    uint16_t response_topic_len;
    const uint8_t *corr_data;
    uint16_t corr_data_len;
    int user_count;
    struct {
        const uint8_t *key;
        uint16_t key_len;
        const uint8_t *val;
        uint16_t val_len;
    } user[CMQ_MQTT_USER_PROPS_MAX];
    size_t consumed;          /* VBI + property bytes */
} cmq_mqtt_props_t;

/* Decode an MQTT 5 Property Length + Properties region.
 * Strings/binary point into buf (borrowed). Returns 0 or -1. */
int cmq_mqtt_props_decode(const uint8_t *buf, size_t len,
                           cmq_mqtt_props_t *props);

/* Payload offset in a PUBLISH variable header. v5=0 skips properties. */
ssize_t cmq_mqtt_publish_payload_off(const uint8_t *vh, size_t vh_len,
                                      int qos, int v5,
                                      cmq_mqtt_props_t *props);

int cmq_mqtt_server_listen(const char *bind_addr, int port);

/* F19: launch the MQTT listener thread on the server. Idempotent
 * in practice; the listener binds 127.0.0.1:1883 and accepts
 * CONNECT / PUBLISH frames, forwarding PUBLISH into the cmq_sublist. */
struct cmq_server;
void cmq_mqtt_server_start_listener(struct cmq_server *server);

/* P1 (v0.5.2): install static credentials for the MQTT listener.
 * When set, CONNECT must include matching Username/Password. NULL
 * or empty strings disable the check (default — listener accepts
 * any CONNECT). */
void cmq_mqtt_set_credentials(const char *user, const char *pass);

/* P3 (v0.5.3): install a file path for persistent retain storage.
 * Retained messages survive restart when this is set. NULL disables. */
void cmq_mqtt_set_retain_path(const char *path);

/* P1 (v0.5.3) F19b full bridge. Set a cmq_server_t* and the bridge
 * becomes live: PUBLISH topics insert into srv->sublist via a relay
 * thread that drains a thread-safe queue. Other cmq clients
 * subscribed to the same topic receive via the existing
 * snapshot_deliver_targets path. Pass NULL to disable. */
void cmq_mqtt_set_bridge_server(struct cmq_server *server);

/* P3 v0.5.4: enable the MQTT listener. Default off — opt-in. */
void cmq_mqtt_set_listener_enabled(int enabled);

/* P1 v0.5.5: per-source-IP rate limit on MQTT PUBLISH. Token
 * bucket of `capacity` per second, refilled at `refill_per_sec`
 * per second. Default off (0) — no limit. */
void cmq_mqtt_set_rate_limit(uint32_t capacity, uint32_t refill_per_sec);

/* P1 v0.5.4: register the cmq_sublist_insert function + sublist
 * pointer so the relay thread can call into cmq_sublist_insert
 * without circular include deps. Called once during cmq_server_create. */
typedef int (*cmq_sublist_insert_fn)(void *sublist, const char *subject,
                                     const uint8_t *payload,
                                     size_t payload_len);
void cmq_mqtt_register_sublist_insert(cmq_sublist_insert_fn fn,
                                       void *sublist);

/* P2 v0.5.4: clean shutdown of the relay thread. Called by
 * cmq_server_destroy. */
void cmq_mqtt_bridge_shutdown(void);

/* v0.5.34: test-only helpers. These bypass the MQTT wire protocol
 * and push directly to the bridge queue. Documented as
 * test-only; production code must not call these. */
int cmq_mqtt_test_enqueue_bridge(const char *topic, const uint8_t *payload,
                                    size_t len);
int cmq_mqtt_test_freelist_count(void);

/* v0.5.25: MQTT 5.0 topic matcher.
 *
 * pattern: SUBSCRIBE filter string, may contain `+` (single-level)
 *   and `#` (multi-level, must be last) wildcards.
 * topic: PUBLISH topic string (no wildcards).
 *
 * Returns:
 *    1  pattern matches topic
 *    0  no match
 *   -1  invalid input (NULL, `#` not at end, multiple `#`, length cap)
 *
 * Pure function; no allocations, no globals, no locks. */
int cmq_mqtt_topic_match(const char *pattern, const char *topic);

/* Record a SUBSCRIBE topic filter. The listener calls this on every
 * accepted SUBSCRIBE. The live bridge uses cmq_mqtt_set_bridge_server. */
int cmq_mqtt_record_subscriber(const char *topic_filter);
int cmq_mqtt_subscriber_count(void);
int cmq_mqtt_get_subscribed_topic(int index, char *out, size_t out_len);

/* P4 (v0.5.2): retained-message store. Last retained payload per
 * topic; cmq_mqtt_fetch_retained returns it for delivery on a new
 * SUBSCRIBE. */
void cmq_mqtt_store_retained(const char *topic, const uint8_t *payload,
                              size_t len);
int cmq_mqtt_fetch_retained(const char *topic, const uint8_t **out,
                             size_t *out_len);

/* Bridge: cmq_mqtt_set_bridge_server wires PUBLISH into
 * cmq_server_publish. cmq_mqtt_fetch_retained remains for retain. */

#ifdef __cplusplus
}
#endif

#endif /* CMQ_MQTT_SERVER_H */
