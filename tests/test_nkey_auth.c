/* v0.5.63: nkey CONNECT signature over CMQNK1|<user>. */
#include "cmq_test.h"
#include "cmq_jwt.h"
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>

static void hex_of(const uint8_t *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int mint_pair(uint8_t pub[32], uint8_t sig[64], const char *user) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!kctx) return -1;
    if (EVP_PKEY_keygen_init(kctx) != 1) {
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(kctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    size_t pub_n = 32;
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &pub_n) != 1) {
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    char msg[320];
    snprintf(msg, sizeof(msg), "CMQNK1|%s", user);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    size_t sig_n = 64;
    int rc = -1;
    if (m && EVP_DigestSignInit(m, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestSign(m, sig, &sig_n, (const uint8_t *)msg, strlen(msg)) == 1)
        rc = 0;
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(kctx);
    return rc;
}

TEST(nkey_auth, hex_roundtrip) {
    uint8_t raw[4] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t out[4];
    ASSERT_EQ(cmq_nkey_hex_decode("deadbeef", out, 4), 0);
    ASSERT(memcmp(out, raw, 4) == 0);
    ASSERT(cmq_nkey_hex_decode("zzzz", out, 2) != 0);
    ASSERT(cmq_nkey_hex_decode("abc", out, 2) != 0);
}

TEST(nkey_auth, verify_user) {
    uint8_t pub[32], sig[64];
    ASSERT_EQ(mint_pair(pub, sig, "leaf1"), 0);
    char pub_hex[65], sig_hex[129];
    hex_of(pub, 32, pub_hex);
    hex_of(sig, 64, sig_hex);
    uint8_t pub2[32];
    ASSERT_EQ(cmq_nkey_hex_decode(pub_hex, pub2, 32), 0);
    ASSERT_EQ(cmq_nkey_verify_user(pub2, "leaf1", sig_hex), 0);
    ASSERT(cmq_nkey_verify_user(pub2, "other", sig_hex) != 0);
    sig_hex[0] = (sig_hex[0] == 'a') ? 'b' : 'a';
    ASSERT(cmq_nkey_verify_user(pub2, "leaf1", sig_hex) != 0);
}

TEST(nkey_auth, reject_empty) {
    uint8_t pub[32] = {0};
    ASSERT(cmq_nkey_verify_user(pub, "", "00") != 0);
    ASSERT(cmq_nkey_verify_user(pub, "u", NULL) != 0);
    ASSERT(cmq_nkey_hex_decode(NULL, pub, 32) != 0);
}

TEST_MAIN()
