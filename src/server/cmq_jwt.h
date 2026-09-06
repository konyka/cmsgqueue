#ifndef CMQ_JWT_H
#define CMQ_JWT_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_JWT_LEEWAY_SEC 60
#define CMQ_JWT_TOKEN_MAX  2048
#define CMQ_NKEY_PUB_LEN   32
#define CMQ_NKEY_SIG_LEN   64

#ifdef __cplusplus
extern "C" {
#endif

/* Verify compact HS256 JWT. now_sec is unix time.
 * 0 ok; -1 bad token/sig/args; -2 iss; -3 exp; -4 nbf.
 * sub_out optional (claim `sub`). */
int cmq_jwt_verify_hs256(const char *token, const char *secret,
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len);

/* Ed25519 verify. pub[32], sig[64]. 0 ok; -1 fail. */
int cmq_nkey_verify(const uint8_t pub[CMQ_NKEY_PUB_LEN],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t sig[CMQ_NKEY_SIG_LEN]);

#ifdef __cplusplus
}
#endif

#endif
