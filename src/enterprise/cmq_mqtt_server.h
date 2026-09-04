#ifndef CMQ_MQTT_SERVER_H
#define CMQ_MQTT_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F19: Server-side MQTT 5.0 listener.
 *
 * STUB. The full server-side MQTT listener (CONNECT/CONNACK/
 * SUBSCRIBE/SUBACK/PUBLISH/PUBACK/PINGREQ/PINGRESP/DISCONNECT
 * state machine) is deferred to v0.4.0. The MQTT bridge (F6)
 * ships a client-side library only; this server-side listener
 * would re-implement mosquitto/NanoMQ at the protocol level.
 *
 * Estimate: XL (2-4 weeks for one engineer to land a protocol-
 * compliant broker, including auth, topic wildcards, retain,
 * QoS 1/2, session state, will-message).
 *
 * The stub returns ENOSYS so callers can detect and document
 * the gap.
 */

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
                                     void *data);
void cmq_mqtt_register_sublist_insert(cmq_sublist_insert_fn fn,
                                       void *sublist);

/* P2 v0.5.4: clean shutdown of the relay thread. Called by
 * cmq_server_destroy. */
void cmq_mqtt_bridge_shutdown(void);

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

/* P1 (v0.5.2): record a SUBSCRIBE topic filter. The listener calls
 * this on every accepted SUBSCRIBE. The cmq-sublist bridge (forwarding
 * matching PUBLISH into cmq_sublist) is v0.6 work; today this only
 * maintains an internal record. */
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

/* P1 (v0.5.3): bridge surface. v0.5.2 only stored retained messages
 * and recorded subscriptions; it never actually routed PUBLISH into
 * cmq_sublist. The full bridge requires server_t* plumbing across
 * the listener pthread — deferred to v0.6 per the WBS. For now
 * this API surface returns the most recent retained payload as a
 * primitive bridge: cmq callers can poll cmq_mqtt_fetch_retained
 * for topics they care about. */

#ifdef __cplusplus
}
#endif

#endif /* CMQ_MQTT_SERVER_H */
