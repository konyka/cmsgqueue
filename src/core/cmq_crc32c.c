/* F9: CRC32C implementation with hardware acceleration.
 *
 * Compile-time selection:
 *   - x86_64 + SSE4.2:    use _mm_crc32_u64 intrinsics
 *   - aarch64 + CRC ext:  use crc32cx instruction
 *   - otherwise:          software bit-by-bit (slow but correct)
 *
 * The hot path uses 64-bit instructions where possible. Tail bytes
 * (when len is not a multiple of 8) are processed byte-by-byte.
 *
 * Conv: standard CRC32C (RFC 3309 / iSCSI) uses init=0xFFFFFFFF and
 * xorout=0xFFFFFFFF. Hardware CRC32 instructions on x86_64 and aarch64
 * use init=0, xorout=0. To produce the standard crc, we XOR the
 * running CRC with 0xFFFFFFFF on entry and exit:
 *   init:  crc_hw = ~crc_in
 *   body:  crc_hw = hw_crc32c(crc_hw, data)
 *   final: crc_out = ~crc_hw
 */

#include "cmq_crc32c.h"
#include "cmq_proto.h"
#include <stddef.h>
#include <string.h>

/* CRC32C polynomial (Castagnoli), reflected. */
#define CMQ_CRC32C_POLY 0x82F63B78u

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#define CMQ_CRC32C_HW 1
#define CMQ_CRC32C_HW_DESC "x86_64 SSE4.2"
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define CMQ_CRC32C_HW 1
#define CMQ_CRC32C_HW_DESC "aarch64 CRC32"
#else
#define CMQ_CRC32C_HW 0
#define CMQ_CRC32C_HW_DESC "software"
#endif

/* Tail processing: when len is not a multiple of 8, process the
 * remainder byte-by-byte. Necessary for both hw and sw paths. */
static uint32_t crc32c_tail(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
#if CMQ_CRC32C_HW && (defined(__x86_64__) || defined(__i386__))
        crc = _mm_crc32_u8(crc, data[i]);
#elif CMQ_CRC32C_HW && defined(__aarch64__)
        crc = __crc32cb(crc, data[i]);
#else
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (CMQ_CRC32C_POLY & (uint32_t)-(int)(crc & 1u));
        }
#endif
    }
    return crc;
}

uint32_t cmq_crc32c_raw(uint32_t crc, const uint8_t *data, size_t len) {
    if (!data || len == 0) return crc;

#if CMQ_CRC32C_HW && (defined(__x86_64__) || defined(__i386__))
    /* Process 8 bytes at a time using SSE4.2 _mm_crc32_u64. */
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, data, sizeof(v));
        crc = (uint32_t)_mm_crc32_u64(crc, v);
        data += 8;
        len -= 8;
    }
    return crc32c_tail(crc, data, len);
#elif CMQ_CRC32C_HW && defined(__aarch64__)
    /* Process 8 bytes at a time using CRC32CX. */
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, data, sizeof(v));
        crc = __crc32cd(crc, v);
        data += 8;
        len -= 8;
    }
    return crc32c_tail(crc, data, len);
#else
    /* Software fallback: bit-by-bit. */
    return crc32c_tail(crc, data, len);
#endif
}

uint32_t cmq_crc32c(uint32_t crc, const uint8_t *data, size_t len) {
    /* Standard CRC32C: init=0xFFFFFFFF, xorout=0xFFFFFFFF. */
    if (!data || len == 0) return crc;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    c = cmq_crc32c_raw(c, data, len);
    return c ^ 0xFFFFFFFFu;
}

int cmq_crc32c_is_hw(void) {
#if CMQ_CRC32C_HW
    return 1;
#else
    return 0;
#endif
}

int cmq_checksum_consume(uint8_t flags, const uint8_t *wire, size_t *len) {
    if (!len)
        return -1;
    if ((flags & (uint8_t)CMQ_FLAG_CHECKSUM) == 0)
        return 0;
    if (!wire || *len < 4)
        return -1;
    size_t n = *len - 4;
    uint32_t expect = (uint32_t)wire[n] |
                      ((uint32_t)wire[n + 1] << 8) |
                      ((uint32_t)wire[n + 2] << 16) |
                      ((uint32_t)wire[n + 3] << 24);
    uint32_t got = cmq_crc32c(0, wire, n);
    if (expect != got)
        return -1;
    *len = n;
    return 0;
}
