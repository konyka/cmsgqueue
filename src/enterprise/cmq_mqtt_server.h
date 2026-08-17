#ifndef CMQ_MQTT_SERVER_H
#define CMQ_MQTT_SERVER_H

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

#ifdef __cplusplus
}
#endif

#endif /* CMQ_MQTT_SERVER_H */
