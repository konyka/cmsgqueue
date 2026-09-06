#define _POSIX_C_SOURCE 200809L
#include "cmq_jwt.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int b64url_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int b64url_decode(const char *in, size_t in_len, uint8_t *out,
                         size_t out_cap, size_t *out_len) {
    if (!in || !out || !out_len) return -1;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=') break;
        int v = b64url_val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return 0;
}

static int json_str_claim(const char *json, size_t n, const char *key,
                          char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) return -1;
    char pat[48];
    int pl = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (pl < 0 || pl >= (int)sizeof(pat)) return -1;
    const char *p = json;
    const char *end = json + n;
    size_t plen = (size_t)pl;
    while (p + plen < end) {
        if (memcmp(p, pat, plen) == 0) {
            p += plen;
            size_t i = 0;
            while (p < end && *p != '"' && i + 1 < out_len) {
                if (*p == '\\') return -1;
                out[i++] = *p++;
            }
            if (p >= end || *p != '"') return -1;
            out[i] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

static int json_u64_claim(const char *json, size_t n, const char *key,
                          uint64_t *out) {
    if (!json || !key || !out) return -1;
    char pat[48];
    int pl = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pl < 0 || pl >= (int)sizeof(pat)) return -1;
    const char *p = json;
    const char *end = json + n;
    size_t plen = (size_t)pl;
    while (p + plen < end) {
        if (memcmp(p, pat, plen) == 0) {
            p += plen;
            if (p < end && *p == '"') return -1;
            uint64_t v = 0;
            int any = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                uint64_t nv = v * 10u + (uint64_t)(*p - '0');
                if (nv < v) return -1;
                v = nv;
                p++;
                any = 1;
            }
            if (!any) return -1;
            *out = v;
            return 0;
        }
        p++;
    }
    return -1;
}

int cmq_jwt_verify_hs256(const char *token, const char *secret,
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len) {
    if (!token || !secret || !secret[0] || !issuer || !issuer[0])
        return -1;
    size_t tlen = strnlen(token, CMQ_JWT_TOKEN_MAX + 1);
    if (tlen == 0 || tlen > CMQ_JWT_TOKEN_MAX) return -1;
    const char *d1 = strchr(token, '.');
    if (!d1) return -1;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2 || strchr(d2 + 1, '.')) return -1;
    size_t hlen = (size_t)(d1 - token);
    size_t plen = (size_t)(d2 - d1 - 1);
    size_t slen = strlen(d2 + 1);
    if (hlen == 0 || plen == 0 || slen == 0) return -1;

    uint8_t hdr[256], pay[1024], sig[64], mac[EVP_MAX_MD_SIZE];
    size_t hdr_n = 0, pay_n = 0, sig_n = 0;
    if (b64url_decode(token, hlen, hdr, sizeof(hdr) - 1, &hdr_n) != 0)
        return -1;
    if (b64url_decode(d1 + 1, plen, pay, sizeof(pay) - 1, &pay_n) != 0)
        return -1;
    if (b64url_decode(d2 + 1, slen, sig, sizeof(sig), &sig_n) != 0)
        return -1;
    hdr[hdr_n] = '\0';
    pay[pay_n] = '\0';

    char alg[16] = {0};
    if (json_str_claim((char *)hdr, hdr_n, "alg", alg, sizeof(alg)) != 0)
        return -1;
    if (strcmp(alg, "HS256") != 0) return -1;

    unsigned int mac_n = 0;
    size_t signing_len = hlen + 1 + plen;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret),
              (const unsigned char *)token, signing_len, mac, &mac_n))
        return -1;
    if (mac_n != sig_n || mac_n == 0) return -1;
    if (CRYPTO_memcmp(mac, sig, mac_n) != 0) return -1;

    char iss[128] = {0};
    if (json_str_claim((char *)pay, pay_n, "iss", iss, sizeof(iss)) != 0)
        return -2;
    if (strcmp(iss, issuer) != 0) return -2;

    unsigned skew = leeway_sec ? leeway_sec : CMQ_JWT_LEEWAY_SEC;
    uint64_t exp = 0;
    if (json_u64_claim((char *)pay, pay_n, "exp", &exp) != 0)
        return -3;
    if (now_sec > exp + (uint64_t)skew) return -3;
    uint64_t nbf = 0;
    if (json_u64_claim((char *)pay, pay_n, "nbf", &nbf) == 0) {
        if (now_sec + (uint64_t)skew < nbf) return -4;
    }
    if (sub_out && sub_len > 0) {
        sub_out[0] = '\0';
        (void)json_str_claim((char *)pay, pay_n, "sub", sub_out, sub_len);
    }
    return 0;
}

int cmq_nkey_verify(const uint8_t pub[CMQ_NKEY_PUB_LEN],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t sig[CMQ_NKEY_SIG_LEN]) {
    if (!pub || !sig || (msg_len > 0 && !msg)) return -1;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 pub, CMQ_NKEY_PUB_LEN);
    if (!pkey) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int rc = -1;
    if (ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(ctx, sig, CMQ_NKEY_SIG_LEN, msg ? msg : (const uint8_t *)"",
                         msg_len) == 1)
        rc = 0;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int cmq_nkey_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    if (!hex || !out || out_len == 0) return -1;
    size_t n = strnlen(hex, out_len * 2 + 1);
    if (n != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int cmq_nkey_verify_user(const uint8_t pub[CMQ_NKEY_PUB_LEN],
                         const char *user, const char *sig_hex) {
    if (!pub || !user || !user[0] || !sig_hex) return -1;
    size_t ulen = strnlen(user, 256);
    if (ulen == 0 || ulen >= 256) return -1;
    uint8_t sig[CMQ_NKEY_SIG_LEN];
    if (cmq_nkey_hex_decode(sig_hex, sig, CMQ_NKEY_SIG_LEN) != 0)
        return -1;
    char msg[8 + 256];
    int n = snprintf(msg, sizeof(msg), "CMQNK1|%s", user);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    return cmq_nkey_verify(pub, (const uint8_t *)msg, (size_t)n, sig);
}
