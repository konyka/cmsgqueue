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
#define CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3

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

#ifdef __cplusplus
}
#endif

#endif
