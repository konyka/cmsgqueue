#ifndef CMQ_JWT_H
#define CMQ_JWT_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_JWT_LEEWAY_SEC 60
#define CMQ_JWT_TOKEN_MAX  2048
#define CMQ_NKEY_PUB_LEN   32
#define CMQ_NKEY_SIG_LEN   64
#define CMQ_JWKS_MAX_KEYS  8
#define CMQ_JWKS_JSON_MAX  4096
#define CMQ_JWKS_KTY_OCT   1
#define CMQ_JWKS_KTY_EC    2
#define CMQ_JWKS_KTY_RSA   3
#define CMQ_JWT_EC_XY_LEN  32
#define CMQ_JWT_RSA_N_MIN  256
#define CMQ_JWT_RSA_N_MAX  512
#define CMQ_JWT_RSA_E_MAX  8

typedef struct {
    char kid[64];
    uint8_t kty;
    uint8_t secret[128];
    size_t slen;
    uint8_t x[CMQ_JWT_EC_XY_LEN];
    uint8_t y[CMQ_JWT_EC_XY_LEN];
    uint8_t n[CMQ_JWT_RSA_N_MAX];
    size_t nlen;
    uint8_t e[CMQ_JWT_RSA_E_MAX];
    size_t elen;
} cmq_jwks_key_t;

typedef struct {
    cmq_jwks_key_t keys[CMQ_JWKS_MAX_KEYS];
    int n;
} cmq_jwks_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Verify compact HS256 JWT. now_sec is unix time.
 * 0 ok; -1 bad token/sig/args; -2 iss; -3 exp; -4 nbf.
 * sub_out optional (claim `sub`). */
int cmq_jwt_verify_hs256(const char *token, const char *secret,
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len);
/* v0.5.90: mint compact HS256. 0 ok. iss/sub reject JSON metachars. */
int cmq_jwt_sign_hs256(const char *secret, const char *issuer,
                       const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len);
int cmq_jwt_verify_hs256_bin(const char *token, const uint8_t *secret,
                             size_t slen, const char *issuer,
                             uint64_t now_sec, unsigned leeway_sec,
                             char *sub_out, size_t sub_len);
int cmq_jwt_header_kid(const char *token, char *out, size_t out_len);
int cmq_jwt_header_alg(const char *token, char *out, size_t out_len);
int cmq_jwt_verify_es256(const char *token, const uint8_t x[CMQ_JWT_EC_XY_LEN],
                         const uint8_t y[CMQ_JWT_EC_XY_LEN],
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len);
/* v0.5.91: mint compact ES256 from P-256 scalar d. JWS is raw R||S. */
int cmq_jwt_sign_es256(const uint8_t d[CMQ_JWT_EC_XY_LEN],
                       const char *issuer, const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len);
int cmq_jwks_parse(const char *json, cmq_jwks_t *out);
int cmq_jwks_lookup(const cmq_jwks_t *j, const char *kid,
                    const uint8_t **secret, size_t *slen);
int cmq_jwks_lookup_ec(const cmq_jwks_t *j, const char *kid,
                       const uint8_t **x, const uint8_t **y);
int cmq_jwt_verify_rs256(const char *token, const uint8_t *n, size_t nlen,
                         const uint8_t *e, size_t elen, const char *issuer,
                         uint64_t now_sec, unsigned leeway_sec,
                         char *sub_out, size_t sub_len);
/* v0.5.91: mint compact RS256 from RSA n/e/d. */
int cmq_jwt_sign_rs256(const uint8_t *n, size_t nlen,
                       const uint8_t *e, size_t elen,
                       const uint8_t *d, size_t dlen,
                       const char *issuer, const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len);
int cmq_jwks_lookup_rsa(const cmq_jwks_t *j, const char *kid,
                        const uint8_t **n, size_t *nlen,
                        const uint8_t **e, size_t *elen);
int cmq_jwt_rsa_decode(const char *n_b64, const char *e_b64,
                       uint8_t *n, size_t *nlen,
                       uint8_t *e, size_t *elen);

/* Ed25519 verify. pub[32], sig[64]. 0 ok; -1 fail. */
int cmq_nkey_verify(const uint8_t pub[CMQ_NKEY_PUB_LEN],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t sig[CMQ_NKEY_SIG_LEN]);

int cmq_nkey_hex_decode(const char *hex, uint8_t *out, size_t out_len);
/* Password is 128 hex chars over message CMQNK1|<user>. */
int cmq_nkey_verify_user(const uint8_t pub[CMQ_NKEY_PUB_LEN],
                         const char *user, const char *sig_hex);
/* NATS nkey: 64 hex or U… public; SU… seed. */
int cmq_nkey_pub_decode(const char *s, uint8_t pub[CMQ_NKEY_PUB_LEN]);
int cmq_nkey_pub_encode(const uint8_t pub[CMQ_NKEY_PUB_LEN], char *out,
                        size_t cap);
int cmq_nkey_seed_decode(const char *s, uint8_t seed[CMQ_NKEY_PUB_LEN]);
int cmq_nkey_seed_to_pub(const uint8_t seed[CMQ_NKEY_PUB_LEN],
                         uint8_t pub[CMQ_NKEY_PUB_LEN]);

#ifdef __cplusplus
}
#endif

#endif
