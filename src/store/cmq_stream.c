#define _POSIX_C_SOURCE 200809L
#include "cmq_stream.h"
#include "cmq_store.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>

#define CMQ_MAX_CONSUMERS 64
#define CMQ_MAX_NAME 128

typedef struct {
    char name[CMQ_MAX_NAME];
    uint64_t acked_seq;
} cmq_consumer_entry_t;

struct cmq_stream {
    char name[CMQ_MAX_NAME];
    cmq_store_t *store;
    cmq_consumer_entry_t consumers[CMQ_MAX_CONSUMERS];
    size_t consumer_count;
    size_t max_msgs;
    size_t max_bytes;
    size_t total_bytes;
    cmq_mutex_t lock;
};

cmq_stream_t *cmq_stream_create(const char *name, size_t max_msgs, size_t max_bytes) {
    if (!name) return NULL;
    cmq_stream_t *s = calloc(1, sizeof(cmq_stream_t));
    if (!s) return NULL;
    strncpy(s->name, name, CMQ_MAX_NAME - 1);
    s->name[CMQ_MAX_NAME - 1] = '\0';
    s->store = cmq_store_create(max_msgs > 0 ? max_msgs : 1024);
    if (!s->store) { free(s); return NULL; }
    s->consumer_count = 0;
    s->max_msgs = max_msgs > 0 ? max_msgs : 1024;
    s->max_bytes = max_bytes;
    s->total_bytes = 0;
    cmq_mutex_init(&s->lock);
    return s;
}

void cmq_stream_destroy(cmq_stream_t *stream) {
    if (!stream) return;
    cmq_store_destroy(stream->store);
    cmq_mutex_destroy(&stream->lock);
    free(stream);
}

const char *cmq_stream_name(cmq_stream_t *stream) {
    return stream ? stream->name : NULL;
}

uint64_t cmq_stream_append(cmq_stream_t *stream, const uint8_t *data, size_t len) {
    if (!stream || !data || len == 0) return 0;
    cmq_mutex_lock(&stream->lock);

    if (stream->max_bytes > 0 && stream->total_bytes + len > stream->max_bytes) {
        uint64_t first = cmq_store_first_seq(stream->store);
        uint64_t last = cmq_store_last_seq(stream->store);
        while (first <= last && stream->total_bytes + len > stream->max_bytes) {
            cmq_store_msg_t msg;
            if (cmq_store_get(stream->store, first, &msg) == 0) {
                stream->total_bytes -= msg.len;
            }
            cmq_store_truncate(stream->store, first + 1);
            first++;
        }
    }

    uint64_t seq = cmq_store_put(stream->store, data, len);
    if (seq > 0) {
        stream->total_bytes += len;
    }

    cmq_mutex_unlock(&stream->lock);
    return seq;
}

int cmq_stream_read(cmq_stream_t *stream, uint64_t seq, cmq_stream_msg_t *out) {
    if (!stream || !out) return -1;
    cmq_store_msg_t msg;
    int rc = cmq_store_get(stream->store, seq, &msg);
    if (rc != 0) return rc;
    out->seq = msg.seq;
    out->data = msg.data;
    out->len = msg.len;
    out->timestamp_ms = msg.timestamp_ms;
    return 0;
}

int cmq_stream_add_consumer(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return -1;
    cmq_mutex_lock(&stream->lock);
    if (stream->consumer_count >= CMQ_MAX_CONSUMERS) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            cmq_mutex_unlock(&stream->lock);
            return 0;
        }
    }
    cmq_consumer_entry_t *c = &stream->consumers[stream->consumer_count++];
    strncpy(c->name, consumer_name, CMQ_MAX_NAME - 1);
    c->name[CMQ_MAX_NAME - 1] = '\0';
    c->acked_seq = 0;
    cmq_mutex_unlock(&stream->lock);
    return 0;
}

cmq_stream_consumer_t cmq_stream_consumer_state(cmq_stream_t *stream,
                                                   const char *consumer_name) {
    cmq_stream_consumer_t state = {0, 0};
    if (!stream || !consumer_name) return state;
    cmq_mutex_lock(&stream->lock);
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            state.consumer_seq = stream->consumers[i].acked_seq;
            uint64_t last = cmq_store_last_seq(stream->store);
            if (last > state.consumer_seq) {
                state.pending_count = (uint32_t)(last - state.consumer_seq);
            }
            break;
        }
    }
    cmq_mutex_unlock(&stream->lock);
    return state;
}

uint64_t cmq_stream_consumer_next(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return 0;
    cmq_mutex_lock(&stream->lock);
    uint64_t next = 0;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            next = stream->consumers[i].acked_seq + 1;
            break;
        }
    }
    cmq_mutex_unlock(&stream->lock);
    return next;
}

int cmq_stream_consumer_ack(cmq_stream_t *stream, const char *consumer_name,
                             uint64_t seq) {
    if (!stream || !consumer_name) return -1;
    cmq_mutex_lock(&stream->lock);
    int found = -1;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            if (seq > stream->consumers[i].acked_seq) {
                stream->consumers[i].acked_seq = seq;
            }
            found = 0;
            break;
        }
    }
    cmq_mutex_unlock(&stream->lock);
    return found;
}

size_t cmq_stream_msg_count(cmq_stream_t *stream) {
    return stream ? cmq_store_count(stream->store) : 0;
}

uint64_t cmq_stream_first_seq(cmq_stream_t *stream) {
    return stream ? cmq_store_first_seq(stream->store) : 0;
}

uint64_t cmq_stream_last_seq(cmq_stream_t *stream) {
    return stream ? cmq_store_last_seq(stream->store) : 0;
}
