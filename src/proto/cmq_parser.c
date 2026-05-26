#include "cmq_parser.h"

#include <stdlib.h>
#include <string.h>

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
    size_t inbuf_len;
    size_t inbuf_cap;

    /* queue of parsed frames */
    cmq_frame_node_t *head;
    cmq_frame_node_t *tail;
};

#define CMQ_MAX_PAYLOAD (16 * 1024 * 1024) /* 16 MB */
#define CMQ_HEADER_LEN (sizeof(cmq_frame_hdr_t))

static cmq_frame_node_t *cmq_push_frame(cmq_parser_t *p, cmq_frame_t *frame) {
    cmq_frame_node_t *node = (cmq_frame_node_t *)malloc(sizeof(*node));
    if (!node) return NULL;
    node->frame = *frame;
    node->next = NULL;
    if (!p->head) {
        p->head = p->tail = node;
    } else {
        p->tail->next = node;
        p->tail = node;
    }
    return node;
}

cmq_parser_t *cmq_parser_create(void) {
    cmq_parser_t *p = (cmq_parser_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->inbuf_cap = 1024;
    p->inbuf = (uint8_t *)malloc(p->inbuf_cap);
    p->inbuf_len = 0;
    p->head = p->tail = NULL;
    return p;
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
    /* reset inbuf */
    if (p->inbuf) {
        free(p->inbuf);
    }
    p->inbuf_cap = 1024;
    p->inbuf = (uint8_t *)malloc(p->inbuf_cap);
    p->inbuf_len = 0;
}

static int ensure_inbuf(cmq_parser_t *p, size_t need) {
    if (need <= p->inbuf_cap) return 0;
    size_t newcap = p->inbuf_cap;
    while (newcap < need) newcap <<= 1;
    uint8_t *nb = (uint8_t *)realloc(p->inbuf, newcap);
    if (!nb) return -1;
    p->inbuf = nb;
    p->inbuf_cap = newcap;
    return 0;
}

int cmq_parser_feed(cmq_parser_t *p, const uint8_t *data, size_t len) {
    if (!p || !data || len == 0) {
        return 0;
    }

    /* fast-path: push into inbuf */
    if (p->inbuf_len + len > p->inbuf_cap) {
        if (ensure_inbuf(p, p->inbuf_len + len) != 0) {
            return -1;
        }
    }
    memcpy(p->inbuf + p->inbuf_len, data, len);
    p->inbuf_len += len;

    int produced = 0;
    /* Defensive pre-validation to catch obviously wrong frames early */
    if (p->inbuf_len >= 3) {
        if (p->inbuf[0] != CMQ_PROTO_MAGIC_0 || p->inbuf[1] != CMQ_PROTO_MAGIC_1) {
            /* invalid magic */
            return -1;
        }
        if (p->inbuf[2] != CMQ_PROTO_VERSION) {
            return -1;
        }
    }

    while (p->inbuf_len >= CMQ_HEADER_LEN) {
        const uint8_t *hb = p->inbuf;
        /* validate header basic fields */
        if (hb[0] != CMQ_PROTO_MAGIC_0 || hb[1] != CMQ_PROTO_MAGIC_1) {
            return -1;
        }
        if (hb[2] != CMQ_PROTO_VERSION) {
            return -1;
        }

        /* payload length is stored in next 4 bytes (little-endian in memory) starting at offset 5 */
        uint32_t payload_len = (uint32_t)hb[5] | ((uint32_t)hb[6] << 8) |
                               ((uint32_t)hb[7] << 16) | ((uint32_t)hb[8] << 24);

        if (payload_len > CMQ_MAX_PAYLOAD) {
            return -1;
        }

        size_t total = CMQ_HEADER_LEN + (size_t)payload_len;
        if (p->inbuf_len < total) {
            break; /* need more data */
        }

        /* Build frame */
        cmq_frame_t frame;
        frame.hdr.magic[0] = hb[0];
        frame.hdr.magic[1] = hb[1];
        frame.hdr.version = hb[2];
        frame.hdr.flags = hb[3];
        frame.hdr.op = hb[4];
        frame.hdr.length = payload_len; /* keep numeric length in header in host byte order */
        frame.payload_len = (size_t)payload_len;
        if (payload_len > 0) {
            frame.payload = (uint8_t *)malloc(payload_len);
            if (!frame.payload) {
                return -1;
            }
            memcpy(frame.payload, p->inbuf + CMQ_HEADER_LEN, payload_len);
        } else {
            frame.payload = NULL;
        }

        if (!cmq_push_frame(p, &frame)) {
            if (frame.payload) free(frame.payload);
            return -1;
        }

        /* consume bytes from inbuf */
        memmove(p->inbuf, p->inbuf + total, p->inbuf_len - total);
        p->inbuf_len -= total;
        produced = 1;
        /* continue loop to possibly parse more frames */
    }

    return produced ? 1 : 0;
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
    if (n->frame.payload) free(n->frame.payload);
    free(n);
    return (p->head != NULL) ? 1 : 0;
}

size_t cmq_frame_encode(uint8_t *buf, size_t buf_size, cmq_op_t op, cmq_u8_t flags, const uint8_t *payload, size_t payload_len) {
    size_t needed = sizeof(cmq_frame_hdr_t) + payload_len;
    if (buf_size < needed) return 0;
    cmq_frame_hdr_t hdr;
    hdr.magic[0] = CMQ_PROTO_MAGIC_0;
    hdr.magic[1] = CMQ_PROTO_MAGIC_1;
    hdr.version = CMQ_PROTO_VERSION;
    hdr.flags = flags;
    hdr.op = op;
    hdr.length = (uint32_t)payload_len; /* store in little-endian memory */
    /* copy header into buffer directly to preserve memory layout */
    memcpy(buf, &hdr, sizeof(hdr));
    if (payload_len > 0 && payload) {
        memcpy(buf + sizeof(hdr), payload, payload_len);
    }
    return sizeof(hdr) + payload_len;
}
