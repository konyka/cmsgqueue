#ifndef CMQ_STREAM_H
#define CMQ_STREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct cmq_stream cmq_stream_t;

typedef struct {
    uint64_t seq;
    uint8_t *data;                  /* owned copy from read; release with cmq_stream_msg_release */
    size_t len;
    uint64_t timestamp_ms;
} cmq_stream_msg_t;

typedef struct {
    uint64_t consumer_seq;
    uint32_t pending_count;
} cmq_stream_consumer_t;

cmq_stream_t *cmq_stream_create(const char *name, size_t max_msgs, size_t max_bytes);
void cmq_stream_destroy(cmq_stream_t *stream);

/* Copy-out under begin_op — never return an interior pointer (destroy UAF). */
int cmq_stream_name(cmq_stream_t *stream, char *out, size_t out_len);
uint64_t cmq_stream_append(cmq_stream_t *stream, const uint8_t *data, size_t len);
int cmq_stream_read(cmq_stream_t *stream, uint64_t seq, cmq_stream_msg_t *out);
void cmq_stream_msg_release(cmq_stream_msg_t *msg);

#define CMQ_STREAM_MAX_PARTS 16

/* v0.5.56: persist consumer acks under dir/{name}.cursors.
 * Default create does no I/O. Missing file is empty. */
int cmq_stream_set_cursor_path(cmq_stream_t *stream, const char *dir);

/* v0.5.87: 1–16 hash partitions. Default 1 (append path unchanged).
 * May change only while the stream is empty. */
int cmq_stream_set_partitions(cmq_stream_t *stream, unsigned n);
unsigned cmq_stream_partitions(cmq_stream_t *stream);
unsigned cmq_stream_partition_of(const uint8_t *key, size_t klen, unsigned n);
uint64_t cmq_stream_append_key(cmq_stream_t *stream, const uint8_t *key,
                               size_t klen, const uint8_t *data, size_t len);
uint64_t cmq_stream_consumer_next_part(cmq_stream_t *stream,
                                       const char *consumer_name, unsigned part);
int cmq_stream_consumer_ack_part(cmq_stream_t *stream,
                                 const char *consumer_name, unsigned part,
                                 uint64_t seq);

int cmq_stream_add_consumer(cmq_stream_t *stream, const char *consumer_name);
/* Drop a consumer so a stopped ack watermark cannot pin retention forever. */
int cmq_stream_remove_consumer(cmq_stream_t *stream, const char *consumer_name);
cmq_stream_consumer_t cmq_stream_consumer_state(cmq_stream_t *stream, const char *consumer_name);
uint64_t cmq_stream_consumer_next(cmq_stream_t *stream, const char *consumer_name);
int cmq_stream_consumer_ack(cmq_stream_t *stream, const char *consumer_name, uint64_t seq);

size_t cmq_stream_msg_count(cmq_stream_t *stream);
uint64_t cmq_stream_first_seq(cmq_stream_t *stream);
uint64_t cmq_stream_last_seq(cmq_stream_t *stream);

#endif
