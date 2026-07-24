#include "cmq_parser.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "cmq_platform.h"
#include "cmq_types.h"
#include "cmq_proto.h"

/* Internal parser state */
typedef struct cmq_frame_node {
    cmq_frame_t frame;
    struct cmq_frame_node *next;
} cmq_frame_node_t;

struct cmq_parser {
    /* simple inbuf to accumulate incoming data */
    uint8_t *inbuf;
    size_t inbuf_off;               /* consume offset (avoid O(n²) memmove) */
    size_t inbuf_len;               /* write end */
    size_t inbuf_cap;

    /* queue of parsed frames */
    cmq_frame_node_t *head;
    cmq_frame_node_t *tail;
    size_t queued;
    size_t queued_bytes;            /* sum of queued payload lengths */

    size_t max_payload;             /* per-connection payload cap */
    int pending_error;              /* fatal after partial queue — drain then die */
};

#define CMQ_MAX_PAYLOAD (16 * 1024 * 1024) /* 16 MB application body ceiling */
/* Wire frame may carry subject/reply/headers beyond the body cap. */
#define CMQ_MAX_FRAME_PAYLOAD (CMQ_MAX_PAYLOAD + 256u * 2u + 65536u + 64u)
#define CMQ_HEADER_LEN (sizeof(cmq_frame_hdr_t))
#define CMQ_PARSER_FRAME_QUEUE_MAX 64
/* Bound queued payload memory ≈ 2× max_payload (not 64×). */
#define CMQ_PARSER_QUEUED_BYTES_FACTOR 2

static cmq_frame_node_t *cmq_push_frame(cmq_parser_t *p, cmq_frame_t *frame) {
    if (p->queued >= CMQ_PARSER_FRAME_QUEUE_MAX) return NULL; /* backpressure */
    size_t budget = p->max_payload;
    if (budget <= SIZE_MAX / CMQ_PARSER_QUEUED_BYTES_FACTOR)
        budget *= CMQ_PARSER_QUEUED_BYTES_FACTOR;
    else
        budget = SIZE_MAX;
    if (p->queued_bytes > budget ||
        frame->payload_len > budget - p->queued_bytes)
        return NULL; /* backpressure */
    cmq_frame_node_t *node = (cmq_frame_node_t *)malloc(sizeof(*node));
    if (!node) {
        p->pending_error = 1; /* OOM is fatal — not retryable backpressure */
        return NULL;
    }
    node->frame = *frame;
    node->next = NULL;
    if (!p->head) {
        p->head = p->tail = node;
    } else {
        p->tail->next = node;
        p->tail = node;
    }
    p->queued++;
    p->queued_bytes += frame->payload_len;
    return node;
}

cmq_parser_t *cmq_parser_create(void) {
    cmq_parser_t *p = (cmq_parser_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->inbuf_cap = 1024;
    p->inbuf = (uint8_t *)malloc(p->inbuf_cap);
    if (!p->inbuf) {
        free(p);
        return NULL;
    }
    p->inbuf_len = 0;
    p->inbuf_off = 0;
    p->head = p->tail = NULL;
    p->queued = 0;
    p->queued_bytes = 0;
    p->max_payload = CMQ_MAX_PAYLOAD;
    p->pending_error = 0;
    return p;
}

void cmq_parser_set_max_payload(cmq_parser_t *p, size_t max_payload) {
    if (!p) return;
    if (max_payload == 0)
        p->max_payload = CMQ_MAX_PAYLOAD;
    else if (max_payload > CMQ_MAX_FRAME_PAYLOAD)
        p->max_payload = CMQ_MAX_FRAME_PAYLOAD;
    else
        p->max_payload = max_payload;
}

void cmq_parser_destroy(cmq_parser_t *p) {
    if (!p) return;
    /* free queued frames */
    cmq_frame_node_t *cur = p->head;
    while (cur) {
        cmq_frame_node_t *n = cur->next;
        if (cur->frame.payload) free(cur->frame.payload);
        free(cur);
        cur = n;
    }
    p->head = p->tail = NULL;
    p->queued = 0;
    p->queued_bytes = 0;
    if (p->inbuf) free(p->inbuf);
    free(p);
}

void cmq_parser_reset(cmq_parser_t *p) {
    if (!p) return;
    /* free queued frames */
    cmq_frame_node_t *cur = p->head;
    while (cur) {
        cmq_frame_node_t *n = cur->next;
        if (cur->frame.payload) free(cur->frame.payload);
        free(cur);
        cur = n;
    }
    p->head = p->tail = NULL;
    p->queued = 0;
    p->queued_bytes = 0;
    p->pending_error = 0;
    /* reset inbuf */
    if (p->inbuf) {
        free(p->inbuf);
    }
    p->inbuf_cap = 1024;
    p->inbuf = (uint8_t *)malloc(p->inbuf_cap);
    if (!p->inbuf) {
        p->inbuf_cap = 0;
    }
    p->inbuf_len = 0;
    p->inbuf_off = 0;
}

static int ensure_inbuf(cmq_parser_t *p, size_t need) {
    if (need <= p->inbuf_cap) return 0;
    size_t newcap = p->inbuf_cap ? p->inbuf_cap : 1024;
    while (newcap < need) {
        if (newcap > SIZE_MAX / 2) return -1;
        newcap <<= 1;
    }
    uint8_t *nb = (uint8_t *)realloc(p->inbuf, newcap);
    if (!nb) return -1;
    p->inbuf = nb;
    p->inbuf_cap = newcap;
    return 0;
}

/* 1 if caller must drain queued frames (this call or prior) before teardown. */
static int parser_have_frames(const cmq_parser_t *p, int produced) {
    return (produced || p->head) ? 1 : -1;
}

/* Parse complete frames from inbuf into the queue. No append. */
static int parser_parse_inbuf(cmq_parser_t *p) {
    int produced = 0;
    /* Early reject on unconsumed prefix (even before a full header).
       Align mid-stream/trailing: pending_error + drain-then-die. */
    if (p->inbuf_len - p->inbuf_off >= 3) {
        const uint8_t *peek = p->inbuf + p->inbuf_off;
        if (peek[0] != CMQ_PROTO_MAGIC_0 || peek[1] != CMQ_PROTO_MAGIC_1 ||
            peek[2] != CMQ_PROTO_VERSION) {
            p->pending_error = 1;
            return parser_have_frames(p, produced);
        }
    }

    while (p->inbuf_len - p->inbuf_off >= CMQ_HEADER_LEN) {
        const uint8_t *hb = p->inbuf + p->inbuf_off;
        /* Align queue-full: keep already-queued frames when trailing bytes
           are corrupt / oversize / OOM (caller drains then tears down). */
        if (hb[0] != CMQ_PROTO_MAGIC_0 || hb[1] != CMQ_PROTO_MAGIC_1) {
            p->pending_error = 1;
            return parser_have_frames(p, produced);
        }
        if (hb[2] != CMQ_PROTO_VERSION) {
            p->pending_error = 1;
            return parser_have_frames(p, produced);
        }

        uint32_t payload_len = (uint32_t)hb[5] | ((uint32_t)hb[6] << 8) |
                               ((uint32_t)hb[7] << 16) | ((uint32_t)hb[8] << 24);

        if (payload_len > p->max_payload) {
            p->pending_error = 1;
            return parser_have_frames(p, produced);
        }

        size_t total = CMQ_HEADER_LEN + (size_t)payload_len;
        if (p->inbuf_len - p->inbuf_off < total) {
            break; /* need more data */
        }

        cmq_frame_t frame;
        frame.hdr.magic[0] = hb[0];
        frame.hdr.magic[1] = hb[1];
        frame.hdr.version = hb[2];
        frame.hdr.flags = hb[3];
        frame.hdr.op = hb[4];
        frame.hdr.length = payload_len;
        frame.payload_len = (size_t)payload_len;
        if (payload_len > 0) {
            frame.payload = (uint8_t *)malloc(payload_len);
            if (!frame.payload) {
                p->pending_error = 1;
                return parser_have_frames(p, produced);
            }
            memcpy(frame.payload, hb + CMQ_HEADER_LEN, payload_len);
        } else {
            frame.payload = NULL;
        }

        if (!cmq_push_frame(p, &frame)) {
            if (frame.payload) free(frame.payload);
            /* Keep already-queued frames for the caller to drain.
               pending_error set only on push OOM (not queue-full). */
            return parser_have_frames(p, produced);
        }

        p->inbuf_off += total;
        produced = 1;
    }

    /* Trailing junk shorter than a full header still poisons the stream. */
    if (p->inbuf_len - p->inbuf_off >= 3) {
        const uint8_t *peek = p->inbuf + p->inbuf_off;
        if (peek[0] != CMQ_PROTO_MAGIC_0 || peek[1] != CMQ_PROTO_MAGIC_1 ||
            peek[2] != CMQ_PROTO_VERSION) {
            p->pending_error = 1;
            return parser_have_frames(p, produced);
        }
    }

    /* Opportunistic compact when offset grows large. */
    if (p->inbuf_off > 0 &&
        (p->inbuf_off >= 4096 || p->inbuf_off * 2 >= p->inbuf_len)) {
        size_t rem = p->inbuf_len - p->inbuf_off;
        if (rem > 0)
            memmove(p->inbuf, p->inbuf + p->inbuf_off, rem);
        p->inbuf_len = rem;
        p->inbuf_off = 0;
    }

    return produced ? 1 : 0;
}

int cmq_parser_feed(cmq_parser_t *p, const uint8_t *data, size_t len) {
    if (!p || !data || len == 0) {
        return 0;
    }
    if (!p->inbuf || p->inbuf_cap == 0)
        return -1;
    /* Fatal already latched — do not append; drain queue then tear down. */
    if (p->pending_error)
        return p->head ? 1 : -1;

    /* Compact before append if remaining + new won't fit without sliding. */
    size_t used = p->inbuf_len - p->inbuf_off;
    /* Hard cap: one max frame + header. Stops incomplete-frame memory DoS.
       If frames are already queued, return 1 (no append) so callers that
       treat rc<0 as teardown still drain first (align pending_error). */
    size_t hard = CMQ_HEADER_LEN + p->max_payload;
    if (used > hard || len > hard - used)
        return p->head ? 1 : -1;
    if (p->inbuf_off > 0 && p->inbuf_off + used + len > p->inbuf_cap) {
        if (used > 0)
            memmove(p->inbuf, p->inbuf + p->inbuf_off, used);
        p->inbuf_len = used;
        p->inbuf_off = 0;
    }
    if (p->inbuf_len + len > p->inbuf_cap) {
        if (ensure_inbuf(p, p->inbuf_len + len) != 0)
            return p->head ? 1 : -1;
    }
    memcpy(p->inbuf + p->inbuf_len, data, len);
    p->inbuf_len += len;

    return parser_parse_inbuf(p);
}

int cmq_parser_drain_inbuf(cmq_parser_t *p) {
    if (!p || !p->inbuf || p->inbuf_cap == 0)
        return -1;
    if (p->pending_error)
        return p->head ? 1 : -1;
    return parser_parse_inbuf(p);
}

int cmq_parser_pending_error(const cmq_parser_t *p) {
    return (p && p->pending_error) ? 1 : 0;
}

const cmq_frame_t *cmq_parser_frame(cmq_parser_t *p) {
    if (!p || !p->head) return NULL;
    return &p->head->frame;
}

int cmq_parser_next(cmq_parser_t *p) {
    if (!p || !p->head) return 0;
    cmq_frame_node_t *n = p->head;
    p->head = n->next;
    if (!p->head) p->tail = NULL;
    if (p->queued > 0) p->queued--;
    if (p->queued_bytes >= n->frame.payload_len)
        p->queued_bytes -= n->frame.payload_len;
    else
        p->queued_bytes = 0;
    if (n->frame.payload) free(n->frame.payload);
    free(n);
    return (p->head != NULL) ? 1 : 0;
}

size_t cmq_frame_encode(uint8_t *buf, size_t buf_size, cmq_op_t op, cmq_u8_t flags, const uint8_t *payload, size_t payload_len) {
    if (payload_len > UINT32_MAX) return 0;
    if (payload_len > 0 && !payload) return 0;
    if (payload_len > SIZE_MAX - sizeof(cmq_frame_hdr_t)) return 0;
    size_t needed = sizeof(cmq_frame_hdr_t) + payload_len;
    if (buf_size < needed) return 0;
    buf[0] = CMQ_PROTO_MAGIC_0;
    buf[1] = CMQ_PROTO_MAGIC_1;
    buf[2] = CMQ_PROTO_VERSION;
    buf[3] = flags;
    buf[4] = (uint8_t)op;
    /* Wire length is little-endian (matches cmq_parser_feed). */
    uint32_t le = (uint32_t)payload_len;
    buf[5] = (uint8_t)(le);
    buf[6] = (uint8_t)(le >> 8);
    buf[7] = (uint8_t)(le >> 16);
    buf[8] = (uint8_t)(le >> 24);
    if (payload_len > 0)
        memcpy(buf + sizeof(cmq_frame_hdr_t), payload, payload_len);
    return sizeof(cmq_frame_hdr_t) + payload_len;
}
