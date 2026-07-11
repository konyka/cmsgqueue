#define _POSIX_C_SOURCE 200809L
#include "cmq_stream.h"
#include "cmq_store.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

    /* Allocate payload before any eviction so OOM cannot drop retained msgs. */
    uint8_t *copy = malloc(len);
    if (!copy) {
        cmq_mutex_unlock(&stream->lock);
        return 0;
    }
    memcpy(copy, data, len);

    /* Do not evict past the slowest consumer's ack watermark. */
    uint64_t last_seq = cmq_store_last_seq(stream->store);
    uint64_t retain_floor = UINT64_MAX;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        uint64_t acked = stream->consumers[i].acked_seq;
        if (acked > last_seq) acked = last_seq; /* clamp stale/bad ack */
        uint64_t floor = acked + 1;
        if (floor < retain_floor) retain_floor = floor;
    }

    if (stream->max_bytes > 0 && stream->total_bytes + len > stream->max_bytes) {
        uint64_t first = cmq_store_first_seq(stream->store);
        uint64_t last = cmq_store_last_seq(stream->store);
        while (first <= last && stream->total_bytes + len > stream->max_bytes) {
            if (first >= retain_floor)
                break; /* unacked — refuse rather than drop */
            cmq_store_msg_t msg;
            if (cmq_store_get(stream->store, first, &msg) != 0)
                break; /* cannot account bytes — stop eviction */
            size_t mlen = msg.len;
            cmq_store_msg_release(&msg);
            if (cmq_store_evict_seq(stream->store, first) != 0)
                break; /* leave total_bytes unchanged */
            if (stream->total_bytes >= mlen)
                stream->total_bytes -= mlen;
            else
                stream->total_bytes = 0;
            first++;
        }
        if (stream->total_bytes + len > stream->max_bytes) {
            free(copy);
            cmq_mutex_unlock(&stream->lock);
            return 0; /* refuse append — do not exceed max_bytes */
        }
    }

    /* Ring wrap: debit bytes only after put succeeds. */
    size_t wrap_debit = 0;
    if (cmq_store_count(stream->store) >= stream->max_msgs) {
        uint64_t first = cmq_store_first_seq(stream->store);
        if (first >= retain_floor) {
            free(copy);
            cmq_mutex_unlock(&stream->lock);
            return 0; /* would overwrite unacked */
        }
        cmq_store_msg_t old;
        if (cmq_store_get(stream->store, first, &old) != 0) {
            /* Cannot debit wrap — refuse rather than inflate total_bytes. */
            free(copy);
            cmq_mutex_unlock(&stream->lock);
            return 0;
        }
        wrap_debit = old.len;
        cmq_store_msg_release(&old);
    }

    uint64_t seq = cmq_store_put_owned(stream->store, copy, len);
    if (seq > 0) {
        if (wrap_debit > 0) {
            if (stream->total_bytes >= wrap_debit)
                stream->total_bytes -= wrap_debit;
            else
                stream->total_bytes = 0;
        }
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
    out->data = msg.data; /* ownership transferred */
    out->len = msg.len;
    out->timestamp_ms = msg.timestamp_ms;
    return 0;
}

void cmq_stream_msg_release(cmq_stream_msg_t *msg) {
    if (!msg) return;
    free(msg->data);
    msg->data = NULL;
    msg->len = 0;
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
    /* Start at the oldest retained seq (evictions before join are skipped). */
    c->acked_seq = 0;
    uint64_t first = cmq_store_first_seq(stream->store);
    if (first > 0)
        c->acked_seq = first - 1;
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
            uint64_t first = cmq_store_first_seq(stream->store);
            uint64_t last = cmq_store_last_seq(stream->store);
            uint64_t base = state.consumer_seq;
            if (first > 0 && base + 1 < first)
                base = first - 1;
            if (last > base) {
                uint64_t pend = last - base;
                state.pending_count = pend > UINT32_MAX
                    ? UINT32_MAX : (uint32_t)pend;
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
            uint64_t first = cmq_store_first_seq(stream->store);
            uint64_t last = cmq_store_last_seq(stream->store);
            if (first > 0 && next < first)
                next = first;
            if (last == 0 || next > last)
                next = 0;
            break;
        }
    }
    cmq_mutex_unlock(&stream->lock);
    return next;
}

int cmq_stream_consumer_ack(cmq_stream_t *stream, const char *consumer_name,
                             uint64_t seq) {
    if (!stream || !consumer_name || seq == 0) return -1;
    cmq_mutex_lock(&stream->lock);
    int found = -1;
    uint64_t last = cmq_store_last_seq(stream->store);
    if (seq > last) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            /* Cumulative ack (JetStream-style): seq advances watermark up to
               last; gap-ack of unread seqs is intentional API. Beyond-last
               rejected above so retain_floor cannot jump past store end. */
            if (seq > stream->consumers[i].acked_seq)
                stream->consumers[i].acked_seq = seq;
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
