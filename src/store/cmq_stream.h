#ifndef CMQ_STREAM_H
#define CMQ_STREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct cmq_stream cmq_stream_t;

typedef struct {
    uint64_t seq;
    const uint8_t *data;
    size_t len;
    uint64_t timestamp_ms;
} cmq_stream_msg_t;

typedef struct {
    uint64_t consumer_seq;
    uint32_t pending_count;
} cmq_stream_consumer_t;

cmq_stream_t *cmq_stream_create(const char *name, size_t max_msgs, size_t max_bytes);
void cmq_stream_destroy(cmq_stream_t *stream);

const char *cmq_stream_name(cmq_stream_t *stream);
uint64_t cmq_stream_append(cmq_stream_t *stream, const uint8_t *data, size_t len);
int cmq_stream_read(cmq_stream_t *stream, uint64_t seq, cmq_stream_msg_t *out);

int cmq_stream_add_consumer(cmq_stream_t *stream, const char *consumer_name);
cmq_stream_consumer_t cmq_stream_consumer_state(cmq_stream_t *stream, const char *consumer_name);
uint64_t cmq_stream_consumer_next(cmq_stream_t *stream, const char *consumer_name);
int cmq_stream_consumer_ack(cmq_stream_t *stream, const char *consumer_name, uint64_t seq);

size_t cmq_stream_msg_count(cmq_stream_t *stream);
uint64_t cmq_stream_first_seq(cmq_stream_t *stream);
uint64_t cmq_stream_last_seq(cmq_stream_t *stream);

#endif
