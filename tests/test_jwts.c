/* v0.5.91: JWT ES256 / RS256 issuing. */
#include "cmq_test.h"
#include "cmq_jwt.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <string.h>

static int gen_p256(uint8_t d[32], uint8_t x[32], uint8_t y[32]) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    EVP_PKEY *pkey = NULL;
    if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
        EVP_PKEY_CTX_set_group_name(kctx, "prime256v1") != 1 ||
        EVP_PKEY_keygen(kctx, &pkey) != 1 || !pkey) {
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    EVP_PKEY_CTX_free(kctx);
    uint8_t pub[65];
    size_t pubn = 0;
    BIGNUM *priv = NULL;
    int rc = -1;
    if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY,
                                        pub, sizeof(pub), &pubn) == 1 &&
        pubn == 65 && pub[0] == 0x04 &&
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &priv) == 1 &&
        priv && BN_bn2binpad(priv, d, 32) == 32) {
        memcpy(x, pub + 1, 32);
        memcpy(y, pub + 33, 32);
        rc = 0;
    }
    BN_free(priv);
    EVP_PKEY_free(pkey);
    return rc;
}

static int bn_to_raw(const BIGNUM *b, uint8_t *out, size_t cap, size_t *olen) {
    if (!b || !out || !olen) return -1;
    int n = BN_num_bytes(b);
    if (n <= 0 || (size_t)n > cap) return -1;
    if (BN_bn2bin(b, out) != n) return -1;
    *olen = (size_t)n;
    return 0;
}

static int gen_rsa2048(uint8_t *n, size_t *nlen, uint8_t *e, size_t *elen,
                       uint8_t *d, size_t *dlen) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    EVP_PKEY *pkey = NULL;
    int bits = 2048;
    OSSL_PARAM kparams[] = {
        OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_RSA_BITS, &bits),
        OSSL_PARAM_construct_end()
    };
    if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
        EVP_PKEY_CTX_set_params(kctx, kparams) != 1 ||
        EVP_PKEY_keygen(kctx, &pkey) != 1 || !pkey) {
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    EVP_PKEY_CTX_free(kctx);
    BIGNUM *bn = NULL, *be = NULL, *bd = NULL;
    int rc = -1;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &bn) == 1 &&
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &be) == 1 &&
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_D, &bd) == 1 &&
        bn_to_raw(bn, n, CMQ_JWT_RSA_N_MAX, nlen) == 0 &&
        bn_to_raw(be, e, CMQ_JWT_RSA_E_MAX, elen) == 0 &&
        bn_to_raw(bd, d, CMQ_JWT_RSA_N_MAX, dlen) == 0)
        rc = 0;
    BN_free(bn);
    BN_free(be);
    BN_free(bd);
    EVP_PKEY_free(pkey);
    return rc;
}

TEST(jwts, es256_roundtrip) {
    uint8_t d[32], x[32], y[32];
    ASSERT_EQ(gen_p256(d, x, y), 0);
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_es256(d, "cmq", "alice", 2000000000ull,
                                 tok, sizeof(tok)), 0);
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_es256(tok, x, y, "cmq", 1700000000ull, 60,
                                   sub, sizeof(sub)), 0);
    ASSERT_STR_EQ(sub, "alice");
}

TEST(jwts, rs256_roundtrip) {
    uint8_t n[CMQ_JWT_RSA_N_MAX], e[CMQ_JWT_RSA_E_MAX], d[CMQ_JWT_RSA_N_MAX];
    size_t nlen = 0, elen = 0, dlen = 0;
    ASSERT_EQ(gen_rsa2048(n, &nlen, e, &elen, d, &dlen), 0);
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_rs256(n, nlen, e, elen, d, dlen, "cmq", "bob",
                                 2000000000ull, tok, sizeof(tok)), 0);
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_rs256(tok, n, nlen, e, elen, "cmq",
                                   1700000000ull, 60, sub, sizeof(sub)), 0);
    ASSERT_STR_EQ(sub, "bob");
}

TEST(jwts, wrong_key) {
    uint8_t d1[32], x1[32], y1[32], d2[32], x2[32], y2[32];
    ASSERT_EQ(gen_p256(d1, x1, y1), 0);
    ASSERT_EQ(gen_p256(d2, x2, y2), 0);
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_es256(d1, "cmq", "carol", 2000000000ull,
                                 tok, sizeof(tok)), 0);
    ASSERT(cmq_jwt_verify_es256(tok, x2, y2, "cmq", 1700000000ull, 60,
                                NULL, 0) != 0);
}

TEST(jwts, reject) {
    char tok[64];
    uint8_t z[32];
    memset(z, 0, sizeof(z));
    ASSERT(cmq_jwt_sign_es256(NULL, "cmq", "a", 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_es256(z, "cmq", "a", 0, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_es256(z, "cm\"q", "a", 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_rs256(NULL, 256, z, 3, z, 256, "cmq", "a", 1,
                              tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_rs256(z, 16, z, 3, z, 16, "cmq", "a", 1,
                              tok, sizeof(tok)) != 0);
}

TEST_MAIN()
