#define _POSIX_C_SOURCE 200809L
#include "cmq_stream.h"
#include "cmq_store.h"
#include "cmq_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define CMQ_MAX_CONSUMERS 64
#define CMQ_MAX_NAME 128

typedef struct {
    char name[CMQ_MAX_NAME];
    uint64_t acked_seq;
    uint64_t acked_part[CMQ_STREAM_MAX_PARTS];
} cmq_consumer_entry_t;

struct cmq_stream {
    char name[CMQ_MAX_NAME];
    cmq_store_t *store;
    cmq_consumer_entry_t consumers[CMQ_MAX_CONSUMERS];
    size_t consumer_count;
    size_t max_msgs;
    size_t max_bytes;
    size_t total_bytes;
    char cursor_path[600]; /* v0.5.56: empty = memory-only */
    unsigned nparts; /* v0.5.87: 1 default */
    uint8_t *msg_part;
    size_t part_cap;
    cmq_mutex_t lock;
    atomic_int in_flight;
    atomic_int dying;
};

static int stream_begin_op(cmq_stream_t *stream) {
    if (atomic_load_explicit(&stream->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&stream->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&stream->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&stream->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void stream_end_op(cmq_stream_t *stream) {
    atomic_fetch_sub_explicit(&stream->in_flight, 1, memory_order_acq_rel);
}

static int cursor_token_safe(const char *s) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static int cursor_dir_safe(const char *dir) {
    if (!dir || !dir[0]) return 0;
    size_t n = strnlen(dir, 512);
    if (n == 0 || n >= 512) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)dir[i];
        if (c == '\\' || c < 0x20 || c == 0x7f) return 0;
    }
    size_t i = 0;
    while (i < n) {
        while (i < n && dir[i] == '/') i++;
        if (i >= n) break;
        size_t start = i;
        while (i < n && dir[i] != '/') i++;
        size_t len = i - start;
        if (len == 1 && dir[start] == '.') return 0;
        if (len == 2 && dir[start] == '.' && dir[start + 1] == '.') return 0;
    }
    return 1;
}

static void consumer_init_parts(cmq_consumer_entry_t *c, uint64_t seq) {
    c->acked_seq = seq;
    for (unsigned p = 0; p < CMQ_STREAM_MAX_PARTS; p++)
        c->acked_part[p] = seq;
}

static void consumer_sync_acked(cmq_consumer_entry_t *c, unsigned nparts) {
    if (!c || nparts <= 1) return;
    uint64_t m = c->acked_part[0];
    for (unsigned p = 1; p < nparts; p++) {
        if (c->acked_part[p] < m)
            m = c->acked_part[p];
    }
    c->acked_seq = m;
}

static unsigned part_at_locked(const cmq_stream_t *stream, uint64_t seq) {
    if (!stream || stream->nparts <= 1 || !stream->msg_part ||
        stream->part_cap == 0 || seq == 0)
        return 0;
    return (unsigned)stream->msg_part[(size_t)(seq - 1) % stream->part_cap];
}

static int parts_alloc_locked(cmq_stream_t *stream, unsigned n) {
    if (!stream || n < 1 || n > CMQ_STREAM_MAX_PARTS) return -1;
    if (n == 1) {
        free(stream->msg_part);
        stream->msg_part = NULL;
        stream->part_cap = 0;
        stream->nparts = 1;
        return 0;
    }
    uint8_t *p = calloc(stream->max_msgs, 1);
    if (!p) return -1;
    free(stream->msg_part);
    stream->msg_part = p;
    stream->part_cap = stream->max_msgs;
    stream->nparts = n;
    return 0;
}

static int cursor_save_locked(cmq_stream_t *stream) {
    if (!stream || !stream->cursor_path[0]) return 0;
    char tmp[616];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", stream->cursor_path) >=
        (int)sizeof(tmp))
        return -1;
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    int ok;
    if (stream->nparts > 1) {
        ok = (fprintf(fp, "CMQC2\n%u\n", stream->nparts) >= 0);
        for (size_t i = 0; ok && i < stream->consumer_count; i++) {
            if (!cursor_token_safe(stream->consumers[i].name))
                continue;
            for (unsigned p = 0; ok && p < stream->nparts; p++) {
                if (fprintf(fp, "%s %u %llu\n", stream->consumers[i].name, p,
                            (unsigned long long)stream->consumers[i].acked_part[p]) < 0)
                    ok = 0;
            }
        }
    } else {
        ok = (fprintf(fp, "CMQC1\n") >= 0);
        for (size_t i = 0; ok && i < stream->consumer_count; i++) {
            if (!cursor_token_safe(stream->consumers[i].name))
                continue;
            if (fprintf(fp, "%s %llu\n", stream->consumers[i].name,
                        (unsigned long long)stream->consumers[i].acked_seq) < 0)
                ok = 0;
        }
    }
    if (ok) ok = (fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    fclose(fp);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, stream->cursor_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static cmq_consumer_entry_t *cursor_find_or_add(cmq_stream_t *stream,
                                                const char *name) {
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, name) == 0)
            return &stream->consumers[i];
    }
    if (stream->consumer_count >= CMQ_MAX_CONSUMERS)
        return NULL;
    cmq_consumer_entry_t *c = &stream->consumers[stream->consumer_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    consumer_init_parts(c, 0);
    return c;
}

static int cursor_load_locked(cmq_stream_t *stream) {
    if (!stream || !stream->cursor_path[0]) return 0;
    FILE *fp = fopen(stream->cursor_path, "rb");
    if (!fp) return 0;
    char line[196];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    if (strncmp(line, "CMQC2", 5) == 0) {
        unsigned fn = 0;
        if (!fgets(line, sizeof(line), fp) ||
            sscanf(line, "%u", &fn) != 1 ||
            fn < 1 || fn > CMQ_STREAM_MAX_PARTS) {
            fclose(fp);
            return -1;
        }
        if (stream->nparts == 1) {
            if (parts_alloc_locked(stream, fn) != 0) {
                fclose(fp);
                return -1;
            }
        } else if (stream->nparts != fn) {
            fclose(fp);
            return -1;
        }
        while (fgets(line, sizeof(line), fp)) {
            char name[CMQ_MAX_NAME];
            unsigned part = 0;
            unsigned long long seq = 0;
            if (sscanf(line, "%127s %u %llu", name, &part, &seq) != 3)
                continue;
            if (!cursor_token_safe(name) || part >= stream->nparts)
                continue;
            cmq_consumer_entry_t *c = cursor_find_or_add(stream, name);
            if (!c) continue;
            if (seq > c->acked_part[part])
                c->acked_part[part] = (uint64_t)seq;
            consumer_sync_acked(c, stream->nparts);
        }
        fclose(fp);
        return 0;
    }
    if (strncmp(line, "CMQC1", 5) != 0) {
        fclose(fp);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char name[CMQ_MAX_NAME];
        unsigned long long seq = 0;
        if (sscanf(line, "%127s %llu", name, &seq) != 2) continue;
        if (!cursor_token_safe(name)) continue;
        cmq_consumer_entry_t *c = cursor_find_or_add(stream, name);
        if (!c) continue;
        if (seq > c->acked_seq)
            consumer_init_parts(c, (uint64_t)seq);
    }
    fclose(fp);
    return 0;
}

int cmq_stream_set_cursor_path(cmq_stream_t *stream, const char *dir) {
    if (!stream || !cursor_dir_safe(dir) || !cursor_token_safe(stream->name))
        return -1;
    if (stream_begin_op(stream) != 0) return -1;
    cmq_mutex_lock(&stream->lock);
    int rc = -1;
    if (snprintf(stream->cursor_path, sizeof(stream->cursor_path),
                 "%s/%s.cursors", dir, stream->name) <
        (int)sizeof(stream->cursor_path)) {
        rc = cursor_load_locked(stream);
        if (rc == 0)
            rc = cursor_save_locked(stream);
    } else {
        stream->cursor_path[0] = '\0';
    }
    cmq_mutex_unlock(&stream->lock);
    stream_end_op(stream);
    return rc;
}

cmq_stream_t *cmq_stream_create(const char *name, size_t max_msgs, size_t max_bytes) {
    if (!name) return NULL;
    if (strnlen(name, CMQ_MAX_NAME) >= CMQ_MAX_NAME) return NULL;
    cmq_stream_t *s = calloc(1, sizeof(cmq_stream_t));
    if (!s) return NULL;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->store = cmq_store_create(max_msgs > 0 ? max_msgs : 1024);
    if (!s->store) { free(s); return NULL; }
    s->consumer_count = 0;
    s->max_msgs = max_msgs > 0 ? max_msgs : 1024;
    s->max_bytes = max_bytes;
    s->total_bytes = 0;
    s->nparts = 1;
    s->msg_part = NULL;
    s->part_cap = 0;
    atomic_init(&s->in_flight, 0);
    atomic_init(&s->dying, 0);
    cmq_mutex_init(&s->lock);
    return s;
}

void cmq_stream_destroy(cmq_stream_t *stream) {
    if (!stream) return;
    atomic_store_explicit(&stream->dying, 1, memory_order_release);
    while (atomic_load_explicit(&stream->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    cmq_store_destroy(stream->store);
    free(stream->msg_part);
    cmq_mutex_destroy(&stream->lock);
    free(stream);
}

int cmq_stream_name(cmq_stream_t *stream, char *out, size_t out_len) {
    if (!stream || !out || out_len == 0) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    cmq_mutex_lock(&stream->lock);
    size_t n = strnlen(stream->name, sizeof(stream->name));
    if (n + 1 > out_len) {
        cmq_mutex_unlock(&stream->lock);
        stream_end_op(stream);
        return -1;
    }
    memcpy(out, stream->name, n);
    out[n] = '\0';
    cmq_mutex_unlock(&stream->lock);
    stream_end_op(stream);
    return 0;
}

static uint64_t consumer_floor_locked(const cmq_stream_t *stream,
                                      const cmq_consumer_entry_t *c,
                                      uint64_t first, uint64_t last) {
    uint64_t floor = UINT64_MAX;
    unsigned n = stream->nparts > 1 ? stream->nparts : 1;
    if (n <= 1) {
        uint64_t acked = c->acked_seq;
        if (acked > last) acked = last;
        floor = acked + 1;
        if (first > 0 && floor < first)
            floor = first;
        return floor;
    }
    for (unsigned p = 0; p < n; p++) {
        uint64_t acked = c->acked_part[p];
        if (acked > last) acked = last;
        uint64_t f = acked + 1;
        if (first > 0 && f < first)
            f = first;
        if (f < floor)
            floor = f;
    }
    return floor;
}

static uint64_t stream_append_impl(cmq_stream_t *stream, const uint8_t *data,
                                   size_t len, unsigned part) {
    if (!stream || !data || len == 0) return 0;

    /* Allocate/copy before stream lock — OOM cannot drop retained msgs. */
    uint8_t *copy = malloc(len);
    if (!copy) return 0;
    memcpy(copy, data, len);

    cmq_mutex_lock(&stream->lock);

    /* Do not evict past the slowest consumer's ack watermark. */
    uint64_t last_seq = cmq_store_last_seq(stream->store);
    uint64_t first_seq = cmq_store_first_seq(stream->store);
    uint64_t retain_floor = UINT64_MAX;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        uint64_t floor = consumer_floor_locked(stream, &stream->consumers[i],
                                               first_seq, last_seq);
        if (floor < retain_floor) retain_floor = floor;
    }

    /* Guard wrap / saturation: never rely on wrapping total_bytes + len. */
    if (stream->max_bytes > 0) {
        int over = (stream->total_bytes >= stream->max_bytes) ||
                   (stream->total_bytes >= SIZE_MAX) ||
                   (len > SIZE_MAX - stream->total_bytes) ||
                   (stream->total_bytes + len > stream->max_bytes);
        if (over) {
            uint64_t first = cmq_store_first_seq(stream->store);
            uint64_t last = cmq_store_last_seq(stream->store);
            while (first <= last) {
                over = (stream->total_bytes >= stream->max_bytes) ||
                       (stream->total_bytes >= SIZE_MAX) ||
                       (len > SIZE_MAX - stream->total_bytes) ||
                       (stream->total_bytes + len > stream->max_bytes);
                if (!over)
                    break;
                if (first >= retain_floor)
                    break; /* unacked — refuse rather than drop */
                size_t mlen = 0;
                if (cmq_store_seq_len(stream->store, first, &mlen) != 0)
                    break; /* cannot account bytes — stop eviction */
                if (cmq_store_evict_seq(stream->store, first) != 0)
                    break; /* leave total_bytes unchanged */
                if (stream->total_bytes >= mlen)
                    stream->total_bytes -= mlen;
                else
                    stream->total_bytes = 0;
                first++;
            }
            over = (stream->total_bytes >= stream->max_bytes) ||
                   (stream->total_bytes >= SIZE_MAX) ||
                   (len > SIZE_MAX - stream->total_bytes) ||
                   (stream->total_bytes + len > stream->max_bytes);
            if (over) {
                free(copy);
                cmq_mutex_unlock(&stream->lock);
                return 0; /* refuse append — do not exceed max_bytes */
            }
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
        size_t wrap_len = 0;
        if (cmq_store_seq_len(stream->store, first, &wrap_len) != 0) {
            /* Cannot debit wrap — refuse rather than inflate total_bytes. */
            free(copy);
            cmq_mutex_unlock(&stream->lock);
            return 0;
        }
        wrap_debit = wrap_len;
    }

    uint64_t seq = cmq_store_put_owned(stream->store, copy, len);
    if (seq > 0) {
        if (stream->msg_part && stream->part_cap > 0)
            stream->msg_part[(size_t)(seq - 1) % stream->part_cap] =
                (uint8_t)part;
        if (wrap_debit > 0) {
            if (stream->total_bytes >= wrap_debit)
                stream->total_bytes -= wrap_debit;
            else
                stream->total_bytes = 0;
        }
        if (len > SIZE_MAX - stream->total_bytes)
            stream->total_bytes = SIZE_MAX;
        else
            stream->total_bytes += len;
    }

    cmq_mutex_unlock(&stream->lock);
    return seq;
}

static int stream_read_impl(cmq_stream_t *stream, uint64_t seq, cmq_stream_msg_t *out) {
    if (!stream || !out) return -1;
    /* Store has its own lock + begin_op barrier — no need to hold stream->lock
       across get (avoids blocking append/ack during large copies). */
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

static int stream_add_consumer_impl(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return -1;
    size_t nlen = strnlen(consumer_name, CMQ_MAX_NAME);
    if (nlen == 0 || nlen >= CMQ_MAX_NAME) return -1;
    cmq_mutex_lock(&stream->lock);
    /* Cap applies to inserts only — full table must still upsert. */
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            cmq_mutex_unlock(&stream->lock);
            return 0;
        }
    }
    if (stream->consumer_count >= CMQ_MAX_CONSUMERS) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    cmq_consumer_entry_t *c = &stream->consumers[stream->consumer_count++];
    snprintf(c->name, sizeof(c->name), "%s", consumer_name);
    /* Start at the oldest retained seq (evictions before join are skipped). */
    uint64_t first = cmq_store_first_seq(stream->store);
    consumer_init_parts(c, first > 0 ? first - 1 : 0);
    cmq_mutex_unlock(&stream->lock);
    return 0;
}

static int stream_remove_consumer_impl(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return -1;
    cmq_mutex_lock(&stream->lock);
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) != 0)
            continue;
        if (i + 1 < stream->consumer_count)
            memmove(&stream->consumers[i], &stream->consumers[i + 1],
                    (stream->consumer_count - i - 1) *
                        sizeof(stream->consumers[0]));
        stream->consumer_count--;
        memset(&stream->consumers[stream->consumer_count], 0,
               sizeof(stream->consumers[0]));
        cmq_mutex_unlock(&stream->lock);
        return 0;
    }
    cmq_mutex_unlock(&stream->lock);
    return -1;
}

static cmq_stream_consumer_t stream_consumer_state_impl(cmq_stream_t *stream,
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

static uint64_t stream_consumer_next_impl(cmq_stream_t *stream, const char *consumer_name) {
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

static int stream_consumer_ack_impl(cmq_stream_t *stream, const char *consumer_name,
                             uint64_t seq) {
    if (!stream || !consumer_name || seq == 0) return -1;
    cmq_mutex_lock(&stream->lock);
    int found = -1;
    uint64_t last = cmq_store_last_seq(stream->store);
    uint64_t first = cmq_store_first_seq(stream->store);
    /* Reject beyond-last and already-evicted seqs (stale ack must not pin). */
    if (seq > last || (first > 0 && seq < first)) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) == 0) {
            /* Cumulative ack (JetStream-style): seq advances watermark up to
               last; gap-ack of unread seqs is intentional API. Beyond-last
               / pre-first rejected above so retain_floor stays in window. */
            if (seq > stream->consumers[i].acked_seq)
                stream->consumers[i].acked_seq = seq;
            if (stream->nparts > 1) {
                unsigned p = part_at_locked(stream, seq);
                if (p < stream->nparts &&
                    seq > stream->consumers[i].acked_part[p])
                    stream->consumers[i].acked_part[p] = seq;
                consumer_sync_acked(&stream->consumers[i], stream->nparts);
            }
            found = 0;
            break;
        }
    }
    cmq_mutex_unlock(&stream->lock);
    return found;
}

static size_t stream_msg_count_impl(cmq_stream_t *stream) {
    if (!stream) return 0;
    cmq_mutex_lock(&stream->lock);
    size_t n = cmq_store_count(stream->store);
    cmq_mutex_unlock(&stream->lock);
    return n;
}

static uint64_t stream_first_seq_impl(cmq_stream_t *stream) {
    if (!stream) return 0;
    cmq_mutex_lock(&stream->lock);
    uint64_t s = cmq_store_first_seq(stream->store);
    cmq_mutex_unlock(&stream->lock);
    return s;
}

static uint64_t stream_last_seq_impl(cmq_stream_t *stream) {
    if (!stream) return 0;
    cmq_mutex_lock(&stream->lock);
    uint64_t s = cmq_store_last_seq(stream->store);
    cmq_mutex_unlock(&stream->lock);
    return s;
}

uint64_t cmq_stream_append(cmq_stream_t *stream, const uint8_t *data, size_t len) {
    if (!stream || !data || len == 0) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    uint64_t seq = stream_append_impl(stream, data, len, 0);
    stream_end_op(stream);
    return seq;
}

int cmq_stream_read(cmq_stream_t *stream, uint64_t seq, cmq_stream_msg_t *out) {
    if (!stream || !out) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    int rc = stream_read_impl(stream, seq, out);
    stream_end_op(stream);
    return rc;
}

int cmq_stream_add_consumer(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    cmq_mutex_lock(&stream->lock);
    int durable = stream->cursor_path[0] != '\0';
    cmq_mutex_unlock(&stream->lock);
    if (durable && !cursor_token_safe(consumer_name)) {
        stream_end_op(stream);
        return -1;
    }
    int rc = stream_add_consumer_impl(stream, consumer_name);
    if (rc == 0) {
        cmq_mutex_lock(&stream->lock);
        if (stream->cursor_path[0])
            (void)cursor_save_locked(stream);
        cmq_mutex_unlock(&stream->lock);
    }
    stream_end_op(stream);
    return rc;
}

int cmq_stream_remove_consumer(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    int rc = stream_remove_consumer_impl(stream, consumer_name);
    if (rc == 0) {
        cmq_mutex_lock(&stream->lock);
        if (stream->cursor_path[0])
            (void)cursor_save_locked(stream);
        cmq_mutex_unlock(&stream->lock);
    }
    stream_end_op(stream);
    return rc;
}

cmq_stream_consumer_t cmq_stream_consumer_state(cmq_stream_t *stream,
                                                   const char *consumer_name) {
    cmq_stream_consumer_t state = {0, 0};
    if (!stream || !consumer_name) return state;
    if (stream_begin_op(stream) != 0) return state;
    state = stream_consumer_state_impl(stream, consumer_name);
    stream_end_op(stream);
    return state;
}

uint64_t cmq_stream_consumer_next(cmq_stream_t *stream, const char *consumer_name) {
    if (!stream || !consumer_name) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    uint64_t n = stream_consumer_next_impl(stream, consumer_name);
    stream_end_op(stream);
    return n;
}

int cmq_stream_consumer_ack(cmq_stream_t *stream, const char *consumer_name,
                             uint64_t seq) {
    if (!stream || !consumer_name || seq == 0) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    int rc = stream_consumer_ack_impl(stream, consumer_name, seq);
    if (rc == 0) {
        cmq_mutex_lock(&stream->lock);
        if (stream->cursor_path[0])
            (void)cursor_save_locked(stream);
        cmq_mutex_unlock(&stream->lock);
    }
    stream_end_op(stream);
    return rc;
}

size_t cmq_stream_msg_count(cmq_stream_t *stream) {
    if (!stream) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    size_t n = stream_msg_count_impl(stream);
    stream_end_op(stream);
    return n;
}

uint64_t cmq_stream_first_seq(cmq_stream_t *stream) {
    if (!stream) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    uint64_t s = stream_first_seq_impl(stream);
    stream_end_op(stream);
    return s;
}

uint64_t cmq_stream_last_seq(cmq_stream_t *stream) {
    if (!stream) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    uint64_t s = stream_last_seq_impl(stream);
    stream_end_op(stream);
    return s;
}

unsigned cmq_stream_partition_of(const uint8_t *key, size_t klen, unsigned n) {
    if (!key || klen == 0 || n < 1)
        return 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < klen; i++) {
        h ^= key[i];
        h *= 16777619u;
    }
    return (unsigned)(h % n);
}

int cmq_stream_set_partitions(cmq_stream_t *stream, unsigned n) {
    if (!stream || n < 1 || n > CMQ_STREAM_MAX_PARTS) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    cmq_mutex_lock(&stream->lock);
    int rc = 0;
    if (stream->nparts != n) {
        if (cmq_store_last_seq(stream->store) != 0)
            rc = -1;
        else if (parts_alloc_locked(stream, n) != 0)
            rc = -1;
        else {
            for (size_t i = 0; i < stream->consumer_count; i++) {
                consumer_init_parts(&stream->consumers[i],
                                    stream->consumers[i].acked_seq);
            }
        }
    }
    cmq_mutex_unlock(&stream->lock);
    stream_end_op(stream);
    return rc;
}

unsigned cmq_stream_partitions(cmq_stream_t *stream) {
    if (!stream) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    unsigned n = stream->nparts;
    stream_end_op(stream);
    return n;
}

uint64_t cmq_stream_append_key(cmq_stream_t *stream, const uint8_t *key,
                               size_t klen, const uint8_t *data, size_t len) {
    if (!stream || !key || klen == 0 || !data || len == 0) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    cmq_mutex_lock(&stream->lock);
    unsigned n = stream->nparts;
    cmq_mutex_unlock(&stream->lock);
    unsigned part = (n > 1) ? cmq_stream_partition_of(key, klen, n) : 0;
    uint64_t seq = stream_append_impl(stream, data, len, part);
    stream_end_op(stream);
    return seq;
}

static uint64_t stream_consumer_next_part_impl(cmq_stream_t *stream,
                                              const char *consumer_name,
                                              unsigned part) {
    if (!stream || !consumer_name) return 0;
    cmq_mutex_lock(&stream->lock);
    if (part >= stream->nparts) {
        cmq_mutex_unlock(&stream->lock);
        return 0;
    }
    uint64_t next = 0;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) != 0)
            continue;
        uint64_t acked = (stream->nparts > 1)
            ? stream->consumers[i].acked_part[part]
            : stream->consumers[i].acked_seq;
        uint64_t start = acked + 1;
        uint64_t first = cmq_store_first_seq(stream->store);
        uint64_t last = cmq_store_last_seq(stream->store);
        if (first > 0 && start < first)
            start = first;
        if (last == 0 || start > last) {
            next = 0;
            break;
        }
        if (stream->nparts <= 1) {
            next = start;
            break;
        }
        for (uint64_t s = start; s <= last; s++) {
            if (part_at_locked(stream, s) == part) {
                next = s;
                break;
            }
        }
        break;
    }
    cmq_mutex_unlock(&stream->lock);
    return next;
}

static int stream_consumer_ack_part_impl(cmq_stream_t *stream,
                                         const char *consumer_name,
                                         unsigned part, uint64_t seq) {
    if (!stream || !consumer_name || seq == 0) return -1;
    cmq_mutex_lock(&stream->lock);
    if (part >= stream->nparts) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    uint64_t last = cmq_store_last_seq(stream->store);
    uint64_t first = cmq_store_first_seq(stream->store);
    if (seq > last || (first > 0 && seq < first)) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    if (stream->nparts > 1 && part_at_locked(stream, seq) != part) {
        cmq_mutex_unlock(&stream->lock);
        return -1;
    }
    int found = -1;
    for (size_t i = 0; i < stream->consumer_count; i++) {
        if (strcmp(stream->consumers[i].name, consumer_name) != 0)
            continue;
        if (stream->nparts > 1) {
            if (seq > stream->consumers[i].acked_part[part])
                stream->consumers[i].acked_part[part] = seq;
            consumer_sync_acked(&stream->consumers[i], stream->nparts);
        } else if (seq > stream->consumers[i].acked_seq) {
            stream->consumers[i].acked_seq = seq;
        }
        found = 0;
        break;
    }
    cmq_mutex_unlock(&stream->lock);
    return found;
}

uint64_t cmq_stream_consumer_next_part(cmq_stream_t *stream,
                                       const char *consumer_name,
                                       unsigned part) {
    if (!stream || !consumer_name) return 0;
    if (stream_begin_op(stream) != 0) return 0;
    uint64_t n = stream_consumer_next_part_impl(stream, consumer_name, part);
    stream_end_op(stream);
    return n;
}

int cmq_stream_consumer_ack_part(cmq_stream_t *stream,
                                 const char *consumer_name, unsigned part,
                                 uint64_t seq) {
    if (!stream || !consumer_name || seq == 0) return -1;
    if (stream_begin_op(stream) != 0) return -1;
    int rc = stream_consumer_ack_part_impl(stream, consumer_name, part, seq);
    if (rc == 0) {
        cmq_mutex_lock(&stream->lock);
        if (stream->cursor_path[0])
            (void)cursor_save_locked(stream);
        cmq_mutex_unlock(&stream->lock);
    }
    stream_end_op(stream);
    return rc;
}

