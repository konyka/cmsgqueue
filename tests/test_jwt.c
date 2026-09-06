/* v0.5.62: D3 JWT HS256 + Ed25519 nkey verify. */
#include "cmq_test.h"
#include "cmq_jwt.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

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
                       const char *payload_json) {
    const char *hdr = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char h[128], p[256];
    b64url_encode((const uint8_t *)hdr, strlen(hdr), h, sizeof(h));
    b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p,
                  sizeof(p));
    char signing[400];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);
    unsigned char mac[32];
    unsigned int mac_n = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)signing, strlen(signing), mac, &mac_n);
    char s[64];
    b64url_encode(mac, mac_n, s, sizeof(s));
    snprintf(out, cap, "%s.%s", signing, s);
}

TEST(jwt, hs256_ok) {
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"iss\":\"cmq\",\"sub\":\"alice\",\"exp\":2000000000}");
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1700000000, 60, sub,
                                    sizeof(sub)),
              0);
    ASSERT_STR_EQ(sub, "alice");
}

TEST(jwt, bad_sig) {
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"iss\":\"cmq\",\"exp\":2000000000}");
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "wrong", "cmq", 1700000000, 60, NULL, 0),
              -1);
}

TEST(jwt, iss_mismatch) {
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"iss\":\"other\",\"exp\":2000000000}");
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1700000000, 60, NULL,
                                    0),
              -2);
}

TEST(jwt, expired) {
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"iss\":\"cmq\",\"exp\":100}");
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1000, 60, NULL, 0),
              -3);
}

TEST(jwt, nbf_future) {
    char tok[512];
    mint_hs256(tok, sizeof(tok), "s3cret",
               "{\"iss\":\"cmq\",\"exp\":2000000000,\"nbf\":1900000000}");
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1700000000, 60, NULL,
                                    0),
              -4);
}

TEST(jwt, reject_alg) {
    const char *hdr = "{\"alg\":\"none\",\"typ\":\"JWT\"}";
    char h[128], p[256];
    b64url_encode((const uint8_t *)hdr, strlen(hdr), h, sizeof(h));
    const char *pay = "{\"iss\":\"cmq\",\"exp\":2000000000}";
    b64url_encode((const uint8_t *)pay, strlen(pay), p, sizeof(p));
    char tok[400];
    snprintf(tok, sizeof(tok), "%s.%s.e30", h, p);
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1700000000, 60, NULL,
                                    0),
              -1);
}

TEST(nkey, ed25519_roundtrip) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    ASSERT_NOT_NULL(kctx);
    ASSERT_EQ(EVP_PKEY_keygen_init(kctx), 1);
    EVP_PKEY *pkey = NULL;
    ASSERT_EQ(EVP_PKEY_keygen(kctx, &pkey), 1);
    uint8_t pub[32], sig[64];
    size_t pub_n = 32, sig_n = 64;
    ASSERT_EQ(EVP_PKEY_get_raw_public_key(pkey, pub, &pub_n), 1);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(EVP_DigestSignInit(m, NULL, NULL, NULL, pkey), 1);
    const uint8_t msg[] = "leaf-hello";
    ASSERT_EQ(EVP_DigestSign(m, sig, &sig_n, msg, sizeof(msg) - 1), 1);
    ASSERT_EQ(cmq_nkey_verify(pub, msg, sizeof(msg) - 1, sig), 0);
    sig[0] ^= 1;
    ASSERT(cmq_nkey_verify(pub, msg, sizeof(msg) - 1, sig) != 0);
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(kctx);
}

TEST_MAIN()
