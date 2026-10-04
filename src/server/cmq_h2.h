#ifndef CMQ_H2_H
#define CMQ_H2_H

#include <stddef.h>
#include <stdint.h>

#define CMQ_H2_PREFACE_LEN  24
#define CMQ_H2_HDR_LEN      9
#define CMQ_H2_MAX_STREAMS  32
#define CMQ_H2_MAX_FRAME    16384
#define CMQ_H2_TYPE_DATA    0
#define CMQ_H2_TYPE_HEADERS 1
#define CMQ_H2_TYPE_SETTINGS 4
#define CMQ_H2_TYPE_GOAWAY  7
#define CMQ_H2_FLAG_ACK     0x01
#define CMQ_H2_FLAG_END_STREAM 0x01
#define CMQ_H2_FLAG_END_HEADERS 0x04
#define CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3
#define CMQ_H2_IO_MS        200
#define CMQ_H2_SUBJECT_MAX  128

#define CMQ_H2_ST_PREFACE  0
#define CMQ_H2_ST_SETTINGS 1
#define CMQ_H2_ST_OPEN     2
#define CMQ_H2_ST_GOAWAY   3

typedef struct cmq_h2 cmq_h2_t;

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t cmq_h2_preface[CMQ_H2_PREFACE_LEN];

cmq_h2_t *cmq_h2_create(void);
void cmq_h2_destroy(cmq_h2_t *h);

int cmq_h2_preface_ok(const uint8_t *p, size_t n);
int cmq_h2_frame_hdr(const uint8_t *in, size_t n, uint32_t *len,
                     uint8_t *type, uint8_t *flags, uint32_t *sid);

/* SETTINGS payload: one MAX_CONCURRENT_STREAMS entry. */
int cmq_h2_settings_encode(uint32_t max_streams, uint8_t *out, size_t cap);
int cmq_h2_settings_decode(const uint8_t *in, size_t n, uint32_t *max_streams);

/* Consume a complete preface or one complete frame. 1 progressed;
 * 0 need more; -1 protocol error (state GOAWAY). */
int cmq_h2_feed(cmq_h2_t *h, const uint8_t *data, size_t n, size_t *used);
int cmq_h2_state(const cmq_h2_t *h);
int cmq_h2_stream_count(const cmq_h2_t *h);

/* Loopback-only prior-knowledge listener. Port 0 picks an ephemeral. */
int cmq_h2_listen(const char *bind_addr, int port);
int cmq_h2_listen_port(int lfd);
/* v0.5.140: 0 / omitted keeps. Out-of-range fails closed.
 * Binds loopback when *lfd < 0. Existing listener is left
 * alone (no accept-fd rebind). */
int cmq_h2_reload_listen(int *lfd, int *live_port, int fresh_port);
/* v0.5.146: empty host and port 0 keep off. Non-IPv4 / bad
 * port fail closed. Binds when *lfd < 0. Existing accept fd
 * is left alone. default_port is used when both ports are 0. */
int cmq_listener_reload_bind(int *lfd, const char **live_host, int *live_port,
                             const char *fresh_host, int fresh_port,
                             int default_port);
int cmq_h2_session(int fd, char *subject, size_t scap, uint8_t *payload,
                   size_t pcap, size_t *plen);
int cmq_h2_accept(int lfd, char *subject, size_t scap, uint8_t *payload,
                  size_t pcap, size_t *plen);

/* v0.5.83: same session over a TLS handshake. */
struct cmq_tls_session;
struct cmq_tls_config;
int cmq_h2_session_tls(struct cmq_tls_session *tls, char *subject,
                       size_t scap, uint8_t *payload, size_t pcap,
                       size_t *plen);
int cmq_h2_accept_tls(int lfd, struct cmq_tls_config *cfg, char *subject,
                      size_t scap, uint8_t *payload, size_t pcap,
                      size_t *plen);

#ifdef __cplusplus
}
#endif

#endif
