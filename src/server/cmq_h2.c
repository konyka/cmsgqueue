#define _POSIX_C_SOURCE 200809L
#include "cmq_h2.h"

#include <stdlib.h>
#include <string.h>

const uint8_t cmq_h2_preface[CMQ_H2_PREFACE_LEN] = {
    'P','R','I',' ','*',' ','H','T','T','P','/','2','.','0','\r','\n',
    '\r','\n','S','M','\r','\n','\r','\n'
};

struct cmq_h2 {
    int state;
    int streams;
    uint8_t used[CMQ_H2_MAX_STREAMS];
    uint32_t ids[CMQ_H2_MAX_STREAMS];
};

cmq_h2_t *cmq_h2_create(void) {
    cmq_h2_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->state = CMQ_H2_ST_PREFACE;
    return h;
}

void cmq_h2_destroy(cmq_h2_t *h) {
    free(h);
}

int cmq_h2_preface_ok(const uint8_t *p, size_t n) {
    if (!p || n != CMQ_H2_PREFACE_LEN) return -1;
    return memcmp(p, cmq_h2_preface, CMQ_H2_PREFACE_LEN) == 0 ? 0 : -1;
}

int cmq_h2_frame_hdr(const uint8_t *in, size_t n, uint32_t *len,
                     uint8_t *type, uint8_t *flags, uint32_t *sid) {
    if (!in || n < CMQ_H2_HDR_LEN || !len || !type || !flags || !sid)
        return -1;
    *len = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
    *type = in[3];
    *flags = in[4];
    *sid = ((uint32_t)(in[5] & 0x7f) << 24) | ((uint32_t)in[6] << 16) |
           ((uint32_t)in[7] << 8) | in[8];
    return 0;
}

int cmq_h2_settings_encode(uint32_t max_streams, uint8_t *out, size_t cap) {
    if (!out || cap < 6 || max_streams == 0 ||
        max_streams > CMQ_H2_MAX_STREAMS)
        return -1;
    out[0] = 0;
    out[1] = CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS;
    out[2] = (uint8_t)((max_streams >> 24) & 0xff);
    out[3] = (uint8_t)((max_streams >> 16) & 0xff);
    out[4] = (uint8_t)((max_streams >> 8) & 0xff);
    out[5] = (uint8_t)(max_streams & 0xff);
    return 6;
}

int cmq_h2_settings_decode(const uint8_t *in, size_t n, uint32_t *max_streams) {
    if (!in || !max_streams || n < 6 || (n % 6) != 0) return -1;
    int found = 0;
    for (size_t i = 0; i < n; i += 6) {
        uint16_t id = ((uint16_t)in[i] << 8) | in[i + 1];
        uint32_t v = ((uint32_t)in[i + 2] << 24) | ((uint32_t)in[i + 3] << 16) |
                     ((uint32_t)in[i + 4] << 8) | in[i + 5];
        if (id == CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS) {
            if (v == 0 || v > CMQ_H2_MAX_STREAMS) return -1;
            *max_streams = v;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static int h2_fail(cmq_h2_t *h) {
    h->state = CMQ_H2_ST_GOAWAY;
    return -1;
}

static int h2_find(cmq_h2_t *h, uint32_t sid) {
    for (int i = 0; i < h->streams; i++) {
        if (h->ids[i] == sid) return i;
    }
    return -1;
}

static int h2_open(cmq_h2_t *h, uint32_t sid) {
    if ((sid & 1u) == 0) return -1;
    if (h2_find(h, sid) >= 0) return 0;
    if (h->streams >= CMQ_H2_MAX_STREAMS) return -1;
    h->ids[h->streams] = sid;
    h->used[h->streams] = 1;
    h->streams++;
    return 0;
}

int cmq_h2_feed(cmq_h2_t *h, const uint8_t *data, size_t n, size_t *used) {
    if (!h || !data || !used) return -1;
    *used = 0;
    if (h->state == CMQ_H2_ST_GOAWAY) return -1;
    if (h->state == CMQ_H2_ST_PREFACE) {
        if (n < CMQ_H2_PREFACE_LEN) return 0;
        if (cmq_h2_preface_ok(data, CMQ_H2_PREFACE_LEN) != 0)
            return h2_fail(h);
        h->state = CMQ_H2_ST_SETTINGS;
        *used = CMQ_H2_PREFACE_LEN;
        return 1;
    }
    if (n < CMQ_H2_HDR_LEN) return 0;
    uint32_t len = 0, sid = 0;
    uint8_t type = 0, flags = 0;
    if (cmq_h2_frame_hdr(data, n, &len, &type, &flags, &sid) != 0)
        return h2_fail(h);
    if (len > CMQ_H2_MAX_FRAME) return h2_fail(h);
    if (n < CMQ_H2_HDR_LEN + len) return 0;
    const uint8_t *pl = data + CMQ_H2_HDR_LEN;
    if (h->state == CMQ_H2_ST_SETTINGS) {
        if (type != CMQ_H2_TYPE_SETTINGS || sid != 0 ||
            (flags & CMQ_H2_FLAG_ACK))
            return h2_fail(h);
        uint32_t ms = CMQ_H2_MAX_STREAMS;
        if (len > 0 && cmq_h2_settings_decode(pl, len, &ms) != 0)
            return h2_fail(h);
        h->state = CMQ_H2_ST_OPEN;
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_GOAWAY) {
        *used = CMQ_H2_HDR_LEN + len;
        return h2_fail(h);
    }
    if (type == CMQ_H2_TYPE_SETTINGS) {
        if (sid != 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_HEADERS) {
        if (h2_open(h, sid) != 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_DATA) {
        if (h2_find(h, sid) < 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    return h2_fail(h);
}

int cmq_h2_state(const cmq_h2_t *h) {
    return h ? h->state : CMQ_H2_ST_GOAWAY;
}

int cmq_h2_stream_count(const cmq_h2_t *h) {
    return h ? h->streams : 0;
}
