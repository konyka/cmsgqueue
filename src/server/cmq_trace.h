#ifndef CMQ_TRACE_H
#define CMQ_TRACE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F11: Connection tracing.
 *
 * Each connection has a 16-byte trace ID. The ID is generated at
 * CONNECT time and propagated through logs and the cmq_client_t
 * struct. The wire format is hex-encoded lowercase (32 chars).
 *
 * IDs are 128-bit random (UUID v4-like without the version bits).
 * Probability of collision is negligible.
 */

void cmq_trace_id(uint8_t out[16]);

/* Hex-encode 16 bytes into out (must be at least 33 bytes for the
 * null terminator). Returns chars written (32 on success). */
int cmq_trace_id_hex(const uint8_t id[16], char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_TRACE_H */
