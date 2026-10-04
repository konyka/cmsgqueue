/* v0.5.77: D3 JWT RS256 + JWKS RSA. */
#include "cmq_test.h"
#include "cmq_jwt.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <stdio.h>
#include <string.h>

static void b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        if (o + 4 >= cap) break;
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = t[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = '\0';
}

static int bn_to_raw(const BIGNUM *b, uint8_t *out, size_t cap, size_t *olen) {
    if (!b || !out || !olen) return -1;
    int n = BN_num_bytes(b);
    if (n <= 0 || (size_t)n > cap) return -1;
    if (BN_bn2bin(b, out) != n) return -1;
    *olen = (size_t)n;
    return 0;
}

static int mint_rs256(char *out, size_t cap, uint8_t *n, size_t *nlen,
                      uint8_t *e, size_t *elen, const char *hdr_json,
                      const char *payload_json) {
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
    BIGNUM *bn = NULL, *be = NULL;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &bn) != 1 ||
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &be) != 1 ||
        bn_to_raw(bn, n, CMQ_JWT_RSA_N_MAX, nlen) != 0 ||
        bn_to_raw(be, e, CMQ_JWT_RSA_E_MAX, elen) != 0) {
        BN_free(bn);
        BN_free(be);
        EVP_PKEY_free(pkey);
        return -1;
    }
    BN_free(bn);
    BN_free(be);
    char h[192], p[256];
    b64url_encode((const uint8_t *)hdr_json, strlen(hdr_json), h, sizeof(h));
    b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p,
                  sizeof(p));
    char signing[512];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    size_t sl = 0;
    uint8_t sig[CMQ_JWT_RSA_N_MAX];
    int rc = -1;
    if (m && EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestSign(m, NULL, &sl, (const unsigned char *)signing,
                       strlen(signing)) == 1 &&
        sl > 0 && sl <= sizeof(sig) &&
        EVP_DigestSign(m, sig, &sl, (const unsigned char *)signing,
                       strlen(signing)) == 1) {
        char sigb[700];
        b64url_encode(sig, sl, sigb, sizeof(sigb));
        snprintf(out, cap, "%s.%s", signing, sigb);
        rc = 0;
    }
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(pkey);
    return rc;
}

TEST(rs256, ok) {
    uint8_t n[CMQ_JWT_RSA_N_MAX], e[CMQ_JWT_RSA_E_MAX];
    size_t nlen = 0, elen = 0;
    char tok[1600];
    ASSERT_EQ(mint_rs256(tok, sizeof(tok), n, &nlen, e, &elen,
                         "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
                         "{\"iss\":\"cmq\",\"sub\":\"rsa\",\"exp\":2000000000}"),
              0);
    char alg[16] = {0};
    ASSERT_EQ(cmq_jwt_header_alg(tok, alg, sizeof(alg)), 0);
    ASSERT_STR_EQ(alg, "RS256");
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_rs256(tok, n, nlen, e, elen, "cmq", 1700000000, 60,
                                   sub, sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "rsa");
}

TEST(rs256, bad_sig) {
    uint8_t n[CMQ_JWT_RSA_N_MAX], e[CMQ_JWT_RSA_E_MAX];
    size_t nlen = 0, elen = 0;
    char tok[1600];
    ASSERT_EQ(mint_rs256(tok, sizeof(tok), n, &nlen, e, &elen,
                         "{\"alg\":\"RS256\"}",
                         "{\"iss\":\"cmq\",\"exp\":2000000000}"),
              0);
    n[0] ^= 1;
    ASSERT(cmq_jwt_verify_rs256(tok, n, nlen, e, elen, "cmq", 1700000000, 60,
                                NULL, 0) < 0);
}

TEST(rs256, jwks_rsa) {
    uint8_t n[CMQ_JWT_RSA_N_MAX], e[CMQ_JWT_RSA_E_MAX];
    size_t nlen = 0, elen = 0;
    char tok[1600];
    ASSERT_EQ(mint_rs256(tok, sizeof(tok), n, &nlen, e, &elen,
                         "{\"alg\":\"RS256\",\"kid\":\"r1\"}",
                         "{\"iss\":\"cmq\",\"sub\":\"bob\",\"exp\":2000000000}"),
              0);
    char nb[800], eb[24];
    b64url_encode(n, nlen, nb, sizeof(nb));
    b64url_encode(e, elen, eb, sizeof(eb));
    char json[1200];
    snprintf(json, sizeof(json),
             "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"r1\",\"n\":\"%s\","
             "\"e\":\"%s\"}]}",
             nb, eb);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_parse(json, &j), 0);
    const uint8_t *pn = NULL, *pe = NULL;
    size_t pnl = 0, pel = 0;
    ASSERT_EQ(cmq_jwks_lookup_rsa(&j, "r1", &pn, &pnl, &pe, &pel), 0);
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_rs256(tok, pn, pnl, pe, pel, "cmq", 1700000000, 60,
                                   sub, sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "bob");
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT(cmq_jwks_lookup(&j, "r1", &sec, &slen) != 0);
}

TEST(rs256, reject) {
    uint8_t n[8], e[3] = {1, 0, 1};
    memset(n, 1, sizeof(n));
    ASSERT(cmq_jwt_verify_rs256(NULL, n, 8, e, 3, "cmq", 1, 60, NULL, 0) < 0);
    ASSERT(cmq_jwks_parse("{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"r1\","
                          "\"k\":\"YQ\"}]}",
                          &(cmq_jwks_t){0}) != 0);
    ASSERT(cmq_jwks_lookup_rsa(NULL, "r1", NULL, NULL, NULL, NULL) < 0);
}

TEST_MAIN()
