#ifndef CMQ_PASSWORD_H
#define CMQ_PASSWORD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F8: scrypt password hashing.
 *
 * Wire format: $scrypt$N=<power-of-2>,r=<block-size>,p=<parallelism>$<salt-b64>$<hash-b64>
 * Default parameters: N=2^14 (16384), r=8, p=1.
 * Derived key length: 32 bytes.
 * Salt length: 16 bytes (b64-encoded as 24 chars).
 * Hash length: 32 bytes (b64-encoded as 44 chars).
 * Total format length: ~120 bytes for a 32-byte password.
 *
 * Legacy format: $plaintext$<password> for transition. Detected by
 * prefix. NOT recommended for production.
 *
 * OpenSSL 3.x's EVP_PBE_scrypt is used. PBKDF2-HMAC-SHA256 is the
 * fallback when scrypt is unavailable.
 */

#define CMQ_PASSWORD_MAX 256

int cmq_password_hash(const char *password, char *out, size_t out_len);

/* Returns 1 on match, 0 on no match, -1 on malformed/garbled input. */
int cmq_password_verify(const char *stored, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_PASSWORD_H */
