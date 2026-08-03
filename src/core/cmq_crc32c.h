#ifndef CMQ_CRC32C_H
#define CMQ_CRC32C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CRC32C (Castagnoli, polynomial 0x1EDC6F41 reflected to 0x82F63B78)
 * for wire-protocol integrity checking.
 *
 * Two entry points:
 *   cmq_crc32c(crc, data, len)
 *     Standard CRC32C with init=0xFFFFFFFF, xorout=0xFFFFFFFF. Pass 0
 *     as the initial CRC for a fresh computation; pass the previous
 *     return value to chain. This is the form used by iSCSI, RFC 3309,
 *     SCTP, BTRFS, ext4, and CMQ's wire-protocol checksum (F3).
 *
 *   cmq_crc32c_raw(crc, data, len)
 *     Raw CRC32C with init=crc, xorout=0. Useful for streaming where
 *     the caller wants to manage the init/xorout explicitly. The
 *     streaming version omits the inner XOR-FFFFFFFF so callers can
 *     chain without paying the constant inverter overhead per call.
 *
 * Hardware acceleration:
 *   x86_64:    SSE4.2 CRC32 (_mm_crc32_u8 / _mm_crc32_u64)
 *   aarch64:   CRC32CX instruction (ARMv8.0-A CRC extension)
 *   fall-back: software bit-by-bit
 *
 * The reference vectors in test_crc32c.c are from RFC 3309 / iSCSI.
 */

uint32_t cmq_crc32c(uint32_t crc, const uint8_t *data, size_t len);
uint32_t cmq_crc32c_raw(uint32_t crc, const uint8_t *data, size_t len);

/* Reports whether the hardware-accelerated path is active. */
int cmq_crc32c_is_hw(void);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_CRC32C_H */
