#ifndef CMQ_COMPRESS_H
#define CMQ_COMPRESS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F2: zstd compression for batch-level wire payloads.
 *
 * The wire format is unchanged from the caller's perspective:
 *   - Producer writes a BATCH frame with CMQ_FLAG_COMPRESSED set.
 *   - The payload is zstd-compressed at level 1.
 *   - Server decompresses BEFORE the pass-1 validation.
 *
 * The header frame's payload_len is the COMPRESSED size; the
 * decompressed size is determined by the zstd decoder.
 *
 * Threshold: payloads below 512 bytes auto-skip compression (zstd
 * overhead exceeds the savings). The decision is made by the
 * producer (encoder), not by the server.
 */

int cmq_compress_is_available(void);

/* Compress `src` (src_len bytes) into `dst` (up to dst_cap bytes).
 * Returns the compressed size on success, -1 on failure. The
 * compressed size is bounded by ZSTD_compressBound(src_len). */
ssize_t cmq_compress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap);

/* Decompress `src` (src_len bytes) into `dst` (up to dst_cap bytes).
 * Returns the decompressed size on success, -1 on failure (corrupt
 * data, decompression bomb, etc.). */
ssize_t cmq_decompress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap);

/* Recommended destination buffer size when compressing. */
size_t cmq_compress_bound(size_t src_len);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_COMPRESS_H */
