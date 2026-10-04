/* v0.5.65: D3 JWKS oct-key cache + kid select. */
#include "cmq_test.h"
#include "cmq_jwt.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>
#include <stdio.h>

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

static void mint_hs256(char *out, size_t cap, const char *secret,
                       const char *hdr_json, const char *payload_json) {
    char h[160], p[256];
    b64url_encode((const uint8_t *)hdr_json, strlen(hdr_json), h, sizeof(h));
    b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p,
                  sizeof(p));
    char signing[512];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);
    unsigned char mac[32];
    unsigned int mac_n = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)signing, strlen(signing), mac, &mac_n);
    char s[64];
    b64url_encode(mac, mac_n, s, sizeof(s));
    snprintf(out, cap, "%s.%s", signing, s);
}

TEST(jwks, parse_lookup) {
    char k[64];
    b64url_encode((const uint8_t *)"s3cret", 6, k, sizeof(k));
    char json[256];
    snprintf(json, sizeof(json),
             "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"alg\":\"HS256\","
             "\"k\":\"%s\"}]}", k);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_parse(json, &j), 0);
    ASSERT_EQ(j.n, 1);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(&j, "k1", &sec, &slen), 0);
    ASSERT_EQ(slen, (size_t)6);
    ASSERT(memcmp(sec, "s3cret", 6) == 0);
    ASSERT(cmq_jwks_lookup(&j, "nope", &sec, &slen) != 0);
}

TEST(jwks, kid_verify) {
    char k[64];
    b64url_encode((const uint8_t *)"s3cret", 6, k, sizeof(k));
    char json[256];
    snprintf(json, sizeof(json),
             "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"k\":\"%s\"}]}", k);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_parse(json, &j), 0);
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"alg\":\"HS256\",\"kid\":\"k1\"}",
               "{\"iss\":\"cmq\",\"sub\":\"bob\",\"exp\":2000000000}");
    char kid[64] = {0};
    ASSERT_EQ(cmq_jwt_header_kid(tok, kid, sizeof(kid)), 0);
    ASSERT_STR_EQ(kid, "k1");
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(&j, kid, &sec, &slen), 0);
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_hs256_bin(tok, sec, slen, "cmq", 1700000000, 60,
                                       sub, sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "bob");
}

TEST(jwks, unknown_kid) {
    char k[64];
    b64url_encode((const uint8_t *)"s3cret", 6, k, sizeof(k));
    char json[256];
    snprintf(json, sizeof(json),
             "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"k\":\"%s\"}]}", k);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_parse(json, &j), 0);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT(cmq_jwks_lookup(&j, "k2", &sec, &slen) != 0);
}

TEST(jwks, reject) {
    cmq_jwks_t j;
    ASSERT(cmq_jwks_parse(NULL, &j) != 0);
    ASSERT(cmq_jwks_parse("{}", &j) != 0);
    ASSERT(cmq_jwks_parse("{\"keys\":[]}", &j) != 0);
    ASSERT(cmq_jwks_parse(
               "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"x\",\"k\":\"YQ\"}]}",
               &j) != 0);
    ASSERT(cmq_jwt_header_kid("noperiods", NULL, 0) != 0);
}

TEST_MAIN()
