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

ssize_t cmq_decompress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap) {
    if (!src || src_len == 0) return -1;
    if (!dst || dst_cap == 0) return -1;
    size_t rc = ZSTD_decompress(dst, dst_cap, src, src_len);
    if (ZSTD_isError(rc)) return -1;
    /* Bomb protection is provided by dst_cap: ZSTD refuses to write
     * past it. Callers must size dst_cap to a sane upper bound
     * (e.g., 16 MiB for BATCH frames). */
    return (ssize_t)rc;
}
