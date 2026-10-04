#ifndef CMQ_MQTT_H
#define CMQ_MQTT_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_MQTT_MAX_BRIDGES 16
#define CMQ_MQTT_TOPIC_MAX   256
#define CMQ_MQTT_CLIENT_ID   64

typedef struct cmq_mqtt_bridge cmq_mqtt_bridge_t;

typedef struct {
    char client_id[CMQ_MQTT_CLIENT_ID];
    char addr[64];
    int port;
    int keepalive_ms;
    int clean_session;
    int connected;
    uint64_t messages_in;
    uint64_t messages_out;
} cmq_mqtt_bridge_info_t;

typedef struct {
    char cmq_subject[CMQ_MQTT_TOPIC_MAX];
    char mqtt_topic[CMQ_MQTT_TOPIC_MAX];
    int qos;
    int active;
} cmq_mqtt_mapping_t;

cmq_mqtt_bridge_t *cmq_mqtt_bridge_create(const char *client_id);
void cmq_mqtt_bridge_destroy(cmq_mqtt_bridge_t *br);

int cmq_mqtt_bridge_connect(cmq_mqtt_bridge_t *br, const char *addr, int port);
int cmq_mqtt_bridge_disconnect(cmq_mqtt_bridge_t *br);
int cmq_mqtt_bridge_is_connected(cmq_mqtt_bridge_t *br);

/* Copy client_id under begin_op (no interior pointer). 0 = ok, -1 = fail. */
int cmq_mqtt_client_id(cmq_mqtt_bridge_t *br, char *out, size_t out_len);
cmq_mqtt_bridge_info_t cmq_mqtt_bridge_info(cmq_mqtt_bridge_t *br);

int cmq_mqtt_add_mapping(cmq_mqtt_bridge_t *br, const char *cmq_subject,
                          const char *mqtt_topic, int qos);
int cmq_mqtt_remove_mapping(cmq_mqtt_bridge_t *br, const char *mqtt_topic);
size_t cmq_mqtt_mapping_count(cmq_mqtt_bridge_t *br);
/* Copy mapping under lock (no interior pointer). 0 = found, -1 = miss. */
int cmq_mqtt_find_mapping(cmq_mqtt_bridge_t *br, const char *mqtt_topic,
                           cmq_mqtt_mapping_t *out);
/* v0.5.126: replace the live map table. n==0 keeps the current maps.
 * n 1–8; empty subject/topic or qos outside 0–2 fails closed. */
int cmq_mqtt_reload_maps(cmq_mqtt_bridge_t *br,
                         const cmq_mqtt_mapping_t *maps, int n);
/* v0.5.136: empty/omitted addr and port 0 keep the current
 * endpoint. Non-IPv4 / bad port fail closed. A new endpoint
 * calls connect (same-endpoint live peer is a no-op). */
int cmq_mqtt_reload_endpoint(cmq_mqtt_bridge_t *br,
                             const char **live_addr, int *live_port,
                             const char *fresh_addr, int fresh_port);
/* v0.5.142: empty/omitted addr and port 0 keep off. Non-IPv4 /
 * bad port fail closed. Creates and dials when *br is NULL.
 * Existing bridge is left to reload_endpoint. */
int cmq_mqtt_reload_attach(cmq_mqtt_bridge_t **br,
                           const char **live_addr, int *live_port,
                           const char *fresh_addr, int fresh_port);
/* 1 forwarded; 0 no mapping; -1 not connected / bad args / I/O. */
int cmq_mqtt_bridge_publish(cmq_mqtt_bridge_t *br, const char *subject,
                            const uint8_t *payload, size_t len);

const char *cmq_mqtt_topic_to_subject(const char *mqtt_topic, char *buf, size_t len);
const char *cmq_mqtt_subject_to_topic(const char *subject, char *buf, size_t len);

int cmq_mqtt_encode_connect(uint8_t *buf, size_t len, const char *client_id,
                             int keepalive, int clean_session);
/* packet_id required and non-zero when qos > 0; ignored for qos 0. */
int cmq_mqtt_encode_publish(uint8_t *buf, size_t len, const char *topic,
                             const uint8_t *payload, size_t payload_len, int qos,
                             uint16_t packet_id);
/* packet_id must be non-zero (MQTT SUBSCRIBE always carries a packet id). */
int cmq_mqtt_encode_subscribe(uint8_t *buf, size_t len, const char *topic,
                               int qos, uint16_t packet_id);
int cmq_mqtt_encode_pingreq(uint8_t *buf, size_t len);

int cmq_mqtt_decode_connack(const uint8_t *buf, size_t len);
int cmq_mqtt_decode_packet_type(const uint8_t *buf, size_t len);

#endif
