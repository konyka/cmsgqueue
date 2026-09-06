/* v0.5.74: D3 JWT ES256 (P-256) + JWKS EC. */
#include "cmq_test.h"
#include "cmq_jwt.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
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

static int mint_es256(char *out, size_t cap, uint8_t x[32], uint8_t y[32],
                      const char *hdr_json, const char *payload_json) {
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
    if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY,
                                        pub, sizeof(pub), &pubn) != 1 ||
        pubn != 65 || pub[0] != 0x04) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    memcpy(x, pub + 1, 32);
    memcpy(y, pub + 33, 32);
    char h[192], p[256];
    b64url_encode((const uint8_t *)hdr_json, strlen(hdr_json), h, sizeof(h));
    b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p,
                  sizeof(p));
    char signing[512];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    size_t sl = 0;
    uint8_t der[144];
    int rc = -1;
    if (m && EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestSign(m, NULL, &sl, (const unsigned char *)signing,
                       strlen(signing)) == 1 &&
        sl > 0 && sl <= sizeof(der) &&
        EVP_DigestSign(m, der, &sl, (const unsigned char *)signing,
                       strlen(signing)) == 1) {
        const unsigned char *pp = der;
        ECDSA_SIG *es = d2i_ECDSA_SIG(NULL, &pp, (long)sl);
        const BIGNUM *r = NULL, *s = NULL;
        uint8_t raw[64];
        if (es) {
            ECDSA_SIG_get0(es, &r, &s);
            if (r && s && BN_bn2binpad(r, raw, 32) == 32 &&
                BN_bn2binpad(s, raw + 32, 32) == 32) {
                char sigb[96];
                b64url_encode(raw, 64, sigb, sizeof(sigb));
                snprintf(out, cap, "%s.%s", signing, sigb);
                rc = 0;
            }
            ECDSA_SIG_free(es);
        }
    }
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(pkey);
    return rc;
}

TEST(es256, ok) {
    uint8_t x[32], y[32];
    char tok[768];
    ASSERT_EQ(mint_es256(tok, sizeof(tok), x, y,
                         "{\"alg\":\"ES256\",\"typ\":\"JWT\"}",
                         "{\"iss\":\"cmq\",\"sub\":\"eve\",\"exp\":2000000000}"),
              0);
    char alg[16] = {0};
    ASSERT_EQ(cmq_jwt_header_alg(tok, alg, sizeof(alg)), 0);
    ASSERT_STR_EQ(alg, "ES256");
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_es256(tok, x, y, "cmq", 1700000000, 60, sub,
                                   sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "eve");
}

TEST(es256, bad_sig) {
    uint8_t x[32], y[32];
    char tok[768];
    ASSERT_EQ(mint_es256(tok, sizeof(tok), x, y,
                         "{\"alg\":\"ES256\"}",
                         "{\"iss\":\"cmq\",\"exp\":2000000000}"),
              0);
    x[0] ^= 1;
    ASSERT(cmq_jwt_verify_es256(tok, x, y, "cmq", 1700000000, 60, NULL, 0) < 0);
}

TEST(es256, jwks_ec) {
    uint8_t x[32], y[32];
    char tok[768];
    ASSERT_EQ(mint_es256(tok, sizeof(tok), x, y,
                         "{\"alg\":\"ES256\",\"kid\":\"e1\"}",
                         "{\"iss\":\"cmq\",\"sub\":\"bob\",\"exp\":2000000000}"),
              0);
    char xb[64], yb[64];
    b64url_encode(x, 32, xb, sizeof(xb));
    b64url_encode(y, 32, yb, sizeof(yb));
    char json[512];
    snprintf(json, sizeof(json),
             "{\"keys\":[{\"kty\":\"EC\",\"crv\":\"P-256\",\"kid\":\"e1\","
             "\"x\":\"%s\",\"y\":\"%s\"}]}",
             xb, yb);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_parse(json, &j), 0);
    ASSERT_EQ(j.n, 1);
    const uint8_t *px = NULL, *py = NULL;
    ASSERT_EQ(cmq_jwks_lookup_ec(&j, "e1", &px, &py), 0);
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_es256(tok, px, py, "cmq", 1700000000, 60, sub,
                                   sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "bob");
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT(cmq_jwks_lookup(&j, "e1", &sec, &slen) != 0);
}

TEST(es256, reject) {
    uint8_t x[32], y[32];
    memset(x, 1, 32);
    memset(y, 2, 32);
    ASSERT(cmq_jwt_verify_es256(NULL, x, y, "cmq", 1, 60, NULL, 0) < 0);
    ASSERT(cmq_jwt_header_alg("noperiods", NULL, 0) < 0);
    char tok[768];
    ASSERT_EQ(mint_es256(tok, sizeof(tok), x, y, "{\"alg\":\"HS256\"}",
                         "{\"iss\":\"cmq\",\"exp\":2000000000}"),
              0);
    ASSERT(cmq_jwt_verify_es256(tok, x, y, "cmq", 1700000000, 60, NULL, 0) <
           0);
    ASSERT(cmq_jwks_parse("{\"keys\":[{\"kty\":\"EC\",\"kid\":\"e1\","
                          "\"crv\":\"P-384\",\"x\":\"AA\",\"y\":\"AA\"}]}",
                          &(cmq_jwks_t){0}) != 0);
}

TEST_MAIN()
