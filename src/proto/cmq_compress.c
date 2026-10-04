#define _POSIX_C_SOURCE 200809L
#include "cmq_compress.h"
#include "cmq_proto.h"
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

int cmq_compress_is_available(void) {
    return 1;
}

size_t cmq_compress_bound(size_t src_len) {
    return ZSTD_compressBound(src_len);
}

ssize_t cmq_compress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap) {
    if (!src || src_len == 0) return -1;
    if (!dst || dst_cap < ZSTD_compressBound(src_len)) return -1;
    size_t rc = ZSTD_compress(dst, dst_cap, src, src_len, 1);
    if (ZSTD_isError(rc)) return -1;
    return (ssize_t)rc;
}

#define CMQ_DECOMPRESS_MAX_BYTES (16u * 1024u * 1024u)

ssize_t cmq_decompress_bound(const uint8_t *src, size_t src_len) {
    if (!src || src_len == 0) return -1;
    unsigned long long sz = ZSTD_getFrameContentSize(src, src_len);
    if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR)
        return -1;
    if (sz == 0 || sz > CMQ_DECOMPRESS_MAX_BYTES) return -1;
    return (ssize_t)sz;
}

ssize_t cmq_decompress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap) {
    if (!src || src_len == 0) return -1;
    if (!dst || dst_cap == 0) return -1;
    size_t rc = ZSTD_decompress(dst, dst_cap, src, src_len);
    if (ZSTD_isError(rc)) return -1;
    /* Bomb protection is provided by dst_cap: ZSTD refuses to write
     * past it. Callers must size dst_cap via cmq_decompress_bound
     * (content size, capped at 16 MiB). */
    return (ssize_t)rc;
}

int cmq_inflate(const uint8_t *src, size_t src_len,
                uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    ssize_t bound = cmq_decompress_bound(src, src_len);
    if (bound < 0) return -1;
    uint8_t *buf = malloc((size_t)bound);
    if (!buf) return -1;
    ssize_t dlen = cmq_decompress(src, src_len, buf, (size_t)bound);
    if (dlen < 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)dlen;
    return 0;
}

int cmq_message_to_publish(const uint8_t *src, size_t src_len,
                           uint8_t **out, size_t *out_len, uint8_t *out_flags) {
    if (!out || !out_len || !out_flags) return -1;
    *out = NULL;
    *out_len = 0;
    *out_flags = 0;
    if (!src || src_len < 12) return -1;
    size_t off = 4;
    uint16_t slen = ((uint16_t)src[off] << 8) | src[off + 1];
    off += 2;
    if (slen == 0 || slen >= 256 || off + slen + 6 > src_len)
        return -1;
    const uint8_t *subj = src + off;
    off += slen;
    uint16_t hlen = ((uint16_t)src[off] << 8) | src[off + 1];
    off += 2;
    if (off + (size_t)hlen + 4 > src_len) return -1;
    const uint8_t *hdrs = src + off;
    off += hlen;
    uint32_t plen = ((uint32_t)src[off] << 24) | ((uint32_t)src[off + 1] << 16) |
                    ((uint32_t)src[off + 2] << 8) | (uint32_t)src[off + 3];
    off += 4;
    if (off + plen != src_len) return -1;
    size_t outn = 4 + (size_t)slen + (hlen ? (2 + (size_t)hlen) : 0) + plen;
    uint8_t *o = malloc(outn);
    if (!o) return -1;
    size_t w = 0;
    o[w++] = (uint8_t)(slen >> 8);
    o[w++] = (uint8_t)slen;
    memcpy(o + w, subj, slen);
    w += slen;
    o[w++] = 0;
    o[w++] = 0;
    if (hlen) {
        o[w++] = (uint8_t)(hlen >> 8);
        o[w++] = (uint8_t)hlen;
        memcpy(o + w, hdrs, hlen);
        w += hlen;
        *out_flags = CMQ_FLAG_HEADERS;
    }
    if (plen)
        memcpy(o + w, src + off, plen);
    *out = o;
    *out_len = outn;
    return 0;
}
