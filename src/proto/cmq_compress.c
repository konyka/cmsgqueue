#define _POSIX_C_SOURCE 200809L
#include "cmq_compress.h"
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
