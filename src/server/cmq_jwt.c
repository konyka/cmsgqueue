#define _POSIX_C_SOURCE 200809L
#include "cmq_jwt.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>
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

static int jwt_check_claims(const char *pay, size_t pay_n, const char *issuer,
                            uint64_t now_sec, unsigned leeway_sec,
                            char *sub_out, size_t sub_len) {
    char iss[128] = {0};
    if (json_str_claim(pay, pay_n, "iss", iss, sizeof(iss)) != 0)
        return -2;
    if (strcmp(iss, issuer) != 0) return -2;

    unsigned skew = leeway_sec ? leeway_sec : CMQ_JWT_LEEWAY_SEC;
    uint64_t exp = 0;
    if (json_u64_claim(pay, pay_n, "exp", &exp) != 0)
        return -3;
    if (now_sec > exp + (uint64_t)skew) return -3;
    uint64_t nbf = 0;
    if (json_u64_claim(pay, pay_n, "nbf", &nbf) == 0) {
        if (now_sec + (uint64_t)skew < nbf) return -4;
    }
    if (sub_out && sub_len > 0) {
        sub_out[0] = '\0';
        (void)json_str_claim(pay, pay_n, "sub", sub_out, sub_len);
    }
    return 0;
}

int cmq_jwt_verify_hs256_bin(const char *token, const uint8_t *secret,
                             size_t slen, const char *issuer,
                             uint64_t now_sec, unsigned leeway_sec,
                             char *sub_out, size_t sub_len) {
    if (!token || !secret || slen == 0 || slen > 128 ||
        !issuer || !issuer[0])
        return -1;
    size_t tlen = strnlen(token, CMQ_JWT_TOKEN_MAX + 1);
    if (tlen == 0 || tlen > CMQ_JWT_TOKEN_MAX) return -1;
    const char *d1 = strchr(token, '.');
    if (!d1) return -1;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2 || strchr(d2 + 1, '.')) return -1;
    size_t hlen = (size_t)(d1 - token);
    size_t plen = (size_t)(d2 - d1 - 1);
    size_t sigl = strlen(d2 + 1);
    if (hlen == 0 || plen == 0 || sigl == 0) return -1;

    uint8_t hdr[256], pay[1024], sig[64], mac[EVP_MAX_MD_SIZE];
    size_t hdr_n = 0, pay_n = 0, sig_n = 0;
    if (b64url_decode(token, hlen, hdr, sizeof(hdr) - 1, &hdr_n) != 0)
        return -1;
    if (b64url_decode(d1 + 1, plen, pay, sizeof(pay) - 1, &pay_n) != 0)
        return -1;
    if (b64url_decode(d2 + 1, sigl, sig, sizeof(sig), &sig_n) != 0)
        return -1;
    hdr[hdr_n] = '\0';
    pay[pay_n] = '\0';

    char alg[16] = {0};
    if (json_str_claim((char *)hdr, hdr_n, "alg", alg, sizeof(alg)) != 0)
        return -1;
    if (strcmp(alg, "HS256") != 0) return -1;

    unsigned int mac_n = 0;
    size_t signing_len = hlen + 1 + plen;
    if (!HMAC(EVP_sha256(), secret, (int)slen,
              (const unsigned char *)token, signing_len, mac, &mac_n))
        return -1;
    if (mac_n != sig_n || mac_n == 0) return -1;
    if (CRYPTO_memcmp(mac, sig, mac_n) != 0) return -1;

    return jwt_check_claims((char *)pay, pay_n, issuer, now_sec, leeway_sec,
                            sub_out, sub_len);
}

int cmq_jwt_verify_hs256(const char *token, const char *secret,
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len) {
    if (!secret || !secret[0]) return -1;
    return cmq_jwt_verify_hs256_bin(token, (const uint8_t *)secret,
                                    strlen(secret), issuer, now_sec,
                                    leeway_sec, sub_out, sub_len);
}

static int jwt_claim_safe(const char *s, size_t maxn) {
    if (!s || !s[0]) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '"' || *p == '\\')
            return 0;
        if (++n >= maxn)
            return 0;
    }
    return 1;
}

static int b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (!in || !out || cap == 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        if (o + 4 >= cap) return -1;
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = t[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = '\0';
    return 0;
}

int cmq_jwt_sign_hs256(const char *secret, const char *issuer,
                       const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len) {
    if (!secret || !secret[0] || !issuer || !sub || !out || out_len == 0)
        return -1;
    size_t slen = strnlen(secret, 129);
    if (slen == 0 || slen > 128) return -1;
    if (!jwt_claim_safe(issuer, 128) || !jwt_claim_safe(sub, 128))
        return -1;
    if (exp_sec == 0) return -1;

    static const char hdr[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char pay[320];
    int pn = snprintf(pay, sizeof(pay),
                      "{\"iss\":\"%s\",\"sub\":\"%s\",\"exp\":%llu}",
                      issuer, sub, (unsigned long long)exp_sec);
    if (pn < 0 || pn >= (int)sizeof(pay)) return -1;

    char hb[128], pb[256];
    if (b64url_encode((const uint8_t *)hdr, sizeof(hdr) - 1, hb,
                      sizeof(hb)) != 0)
        return -1;
    if (b64url_encode((const uint8_t *)pay, (size_t)pn, pb, sizeof(pb)) != 0)
        return -1;

    char signing[400];
    int sn = snprintf(signing, sizeof(signing), "%s.%s", hb, pb);
    if (sn < 0 || sn >= (int)sizeof(signing)) return -1;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_n = 0;
    if (!HMAC(EVP_sha256(), secret, (int)slen,
              (const unsigned char *)signing, (size_t)sn, mac, &mac_n) ||
        mac_n == 0)
        return -1;

    char sb[64];
    if (b64url_encode(mac, mac_n, sb, sizeof(sb)) != 0)
        return -1;
    int n = snprintf(out, out_len, "%s.%s", signing, sb);
    if (n < 0 || (size_t)n >= out_len || (size_t)n > CMQ_JWT_TOKEN_MAX)
        return -1;
    return 0;
}

static int jwt_make_signing(const char *alg, const char *issuer, const char *sub,
                            uint64_t exp_sec, char *signing, size_t scap) {
    if (!alg || !issuer || !sub || !signing || scap == 0)
        return -1;
    if (!jwt_claim_safe(issuer, 128) || !jwt_claim_safe(sub, 128))
        return -1;
    if (exp_sec == 0) return -1;
    char hdr[48];
    int hn = snprintf(hdr, sizeof(hdr), "{\"alg\":\"%s\",\"typ\":\"JWT\"}", alg);
    if (hn < 0 || hn >= (int)sizeof(hdr)) return -1;
    char pay[320];
    int pn = snprintf(pay, sizeof(pay),
                      "{\"iss\":\"%s\",\"sub\":\"%s\",\"exp\":%llu}",
                      issuer, sub, (unsigned long long)exp_sec);
    if (pn < 0 || pn >= (int)sizeof(pay)) return -1;
    char hb[128], pb[256];
    if (b64url_encode((const uint8_t *)hdr, (size_t)hn, hb, sizeof(hb)) != 0)
        return -1;
    if (b64url_encode((const uint8_t *)pay, (size_t)pn, pb, sizeof(pb)) != 0)
        return -1;
    int sn = snprintf(signing, scap, "%s.%s", hb, pb);
    if (sn < 0 || (size_t)sn >= scap) return -1;
    return 0;
}

static int jwt_finish_token(const char *signing, const uint8_t *sig, size_t sig_n,
                            char *out, size_t out_len) {
    if (!signing || !sig || sig_n == 0 || !out || out_len == 0)
        return -1;
    char sb[700];
    if (b64url_encode(sig, sig_n, sb, sizeof(sb)) != 0)
        return -1;
    int n = snprintf(out, out_len, "%s.%s", signing, sb);
    if (n < 0 || (size_t)n >= out_len || (size_t)n > CMQ_JWT_TOKEN_MAX)
        return -1;
    return 0;
}

int cmq_jwt_sign_es256(const uint8_t d[CMQ_JWT_EC_XY_LEN],
                       const char *issuer, const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len) {
    if (!d || !issuer || !sub || !out || out_len == 0) return -1;
    char signing[400];
    if (jwt_make_signing("ES256", issuer, sub, exp_sec, signing,
                         sizeof(signing)) != 0)
        return -1;

    BIGNUM *bd = BN_bin2bn(d, CMQ_JWT_EC_XY_LEN, NULL);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY *pkey = NULL;
    int rc = -1;
    if (bd && !BN_is_zero(bd) && pctx && bld &&
        EVP_PKEY_fromdata_init(pctx) == 1 &&
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                        "prime256v1", 0) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bd))
        params = OSSL_PARAM_BLD_to_param(bld);
    if (params &&
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1 &&
        pkey) {
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        size_t sl = 0;
        uint8_t der[144];
        if (m &&
            EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
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
                    BN_bn2binpad(s, raw + 32, 32) == 32)
                    rc = jwt_finish_token(signing, raw, 64, out, out_len);
                ECDSA_SIG_free(es);
            }
        }
        EVP_MD_CTX_free(m);
    }
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    BN_free(bd);
    return rc;
}

int cmq_jwt_sign_rs256(const uint8_t *n, size_t nlen,
                       const uint8_t *e, size_t elen,
                       const uint8_t *d, size_t dlen,
                       const char *issuer, const char *sub, uint64_t exp_sec,
                       char *out, size_t out_len) {
    if (!n || !e || !d || !issuer || !sub || !out || out_len == 0)
        return -1;
    if (nlen < CMQ_JWT_RSA_N_MIN || nlen > CMQ_JWT_RSA_N_MAX) return -1;
    if (elen == 0 || elen > CMQ_JWT_RSA_E_MAX) return -1;
    if (dlen == 0 || dlen > CMQ_JWT_RSA_N_MAX) return -1;
    char signing[400];
    if (jwt_make_signing("RS256", issuer, sub, exp_sec, signing,
                         sizeof(signing)) != 0)
        return -1;

    BIGNUM *bn = BN_bin2bn(n, (int)nlen, NULL);
    BIGNUM *be = BN_bin2bn(e, (int)elen, NULL);
    BIGNUM *bd = BN_bin2bn(d, (int)dlen, NULL);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY *pkey = NULL;
    int rc = -1;
    if (bn && be && bd && !BN_is_zero(bd) && pctx && bld &&
        EVP_PKEY_fromdata_init(pctx) == 1 &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_D, bd))
        params = OSSL_PARAM_BLD_to_param(bld);
    if (params &&
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1 &&
        pkey) {
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        size_t sl = 0;
        uint8_t sig[CMQ_JWT_RSA_N_MAX];
        if (m &&
            EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
            EVP_DigestSign(m, NULL, &sl, (const unsigned char *)signing,
                           strlen(signing)) == 1 &&
            sl > 0 && sl <= sizeof(sig) &&
            EVP_DigestSign(m, sig, &sl, (const unsigned char *)signing,
                           strlen(signing)) == 1)
            rc = jwt_finish_token(signing, sig, sl, out, out_len);
        EVP_MD_CTX_free(m);
    }
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    BN_free(bn);
    BN_free(be);
    BN_free(bd);
    return rc;
}

static int jwt_header_claim(const char *token, const char *key, char *out,
                            size_t out_len) {
    if (!token || !key || !out || out_len == 0) return -1;
    const char *d1 = strchr(token, '.');
    if (!d1) return -1;
    size_t hlen = (size_t)(d1 - token);
    if (hlen == 0 || hlen > CMQ_JWT_TOKEN_MAX) return -1;
    uint8_t hdr[256];
    size_t hdr_n = 0;
    if (b64url_decode(token, hlen, hdr, sizeof(hdr) - 1, &hdr_n) != 0)
        return -1;
    hdr[hdr_n] = '\0';
    return json_str_claim((char *)hdr, hdr_n, key, out, out_len);
}

int cmq_jwt_header_kid(const char *token, char *out, size_t out_len) {
    return jwt_header_claim(token, "kid", out, out_len);
}

int cmq_jwt_header_alg(const char *token, char *out, size_t out_len) {
    return jwt_header_claim(token, "alg", out, out_len);
}

int cmq_jwt_verify_es256(const char *token, const uint8_t x[CMQ_JWT_EC_XY_LEN],
                         const uint8_t y[CMQ_JWT_EC_XY_LEN],
                         const char *issuer, uint64_t now_sec,
                         unsigned leeway_sec, char *sub_out, size_t sub_len) {
    if (!token || !x || !y || !issuer || !issuer[0]) return -1;
    size_t tlen = strnlen(token, CMQ_JWT_TOKEN_MAX + 1);
    if (tlen == 0 || tlen > CMQ_JWT_TOKEN_MAX) return -1;
    const char *d1 = strchr(token, '.');
    if (!d1) return -1;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2 || strchr(d2 + 1, '.')) return -1;
    size_t hlen = (size_t)(d1 - token);
    size_t plen = (size_t)(d2 - d1 - 1);
    size_t sigl = strlen(d2 + 1);
    if (hlen == 0 || plen == 0 || sigl == 0) return -1;

    uint8_t hdr[256], pay[1024], sig[80];
    size_t hdr_n = 0, pay_n = 0, sig_n = 0;
    if (b64url_decode(token, hlen, hdr, sizeof(hdr) - 1, &hdr_n) != 0)
        return -1;
    if (b64url_decode(d1 + 1, plen, pay, sizeof(pay) - 1, &pay_n) != 0)
        return -1;
    if (b64url_decode(d2 + 1, sigl, sig, sizeof(sig), &sig_n) != 0)
        return -1;
    hdr[hdr_n] = '\0';
    pay[pay_n] = '\0';
    char alg[16] = {0};
    if (json_str_claim((char *)hdr, hdr_n, "alg", alg, sizeof(alg)) != 0)
        return -1;
    if (strcmp(alg, "ES256") != 0) return -1;
    if (sig_n != 64) return -1;

    uint8_t pub[65];
    pub[0] = 0x04;
    memcpy(pub + 1, x, CMQ_JWT_EC_XY_LEN);
    memcpy(pub + 1 + CMQ_JWT_EC_XY_LEN, y, CMQ_JWT_EC_XY_LEN);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!pctx) return -1;
    EVP_PKEY *pkey = NULL;
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    int rc = -1;
    if (bld && EVP_PKEY_fromdata_init(pctx) == 1 &&
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                        "prime256v1", 0) &&
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub,
                                         sizeof(pub)))
        params = OSSL_PARAM_BLD_to_param(bld);
    if (params &&
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1 &&
        pkey) {
        ECDSA_SIG *es = ECDSA_SIG_new();
        BIGNUM *r = BN_bin2bn(sig, 32, NULL);
        BIGNUM *s = BN_bin2bn(sig + 32, 32, NULL);
        unsigned char *der = NULL;
        int dlen = 0;
        if (es && r && s && ECDSA_SIG_set0(es, r, s) == 1) {
            r = NULL;
            s = NULL;
            dlen = i2d_ECDSA_SIG(es, &der);
        }
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(es);
        if (der && dlen > 0) {
            EVP_MD_CTX *m = EVP_MD_CTX_new();
            size_t signing_len = hlen + 1 + plen;
            if (m &&
                EVP_DigestVerifyInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
                EVP_DigestVerify(m, der, (size_t)dlen,
                                 (const unsigned char *)token, signing_len) == 1)
                rc = jwt_check_claims((char *)pay, pay_n, issuer, now_sec,
                                      leeway_sec, sub_out, sub_len);
            EVP_MD_CTX_free(m);
        }
        OPENSSL_free(der);
    }
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    return rc;
}

int cmq_jwt_rsa_decode(const char *n_b64, const char *e_b64,
                       uint8_t *n, size_t *nlen,
                       uint8_t *e, size_t *elen) {
    if (!n_b64 || !e_b64 || !n || !nlen || !e || !elen) return -1;
    size_t nn = 0, ne = 0;
    if (b64url_decode(n_b64, strlen(n_b64), n, CMQ_JWT_RSA_N_MAX, &nn) != 0)
        return -1;
    if (nn < CMQ_JWT_RSA_N_MIN || nn > CMQ_JWT_RSA_N_MAX) return -1;
    if (b64url_decode(e_b64, strlen(e_b64), e, CMQ_JWT_RSA_E_MAX, &ne) != 0)
        return -1;
    if (ne == 0 || ne > CMQ_JWT_RSA_E_MAX) return -1;
    *nlen = nn;
    *elen = ne;
    return 0;
}

int cmq_jwt_verify_rs256(const char *token, const uint8_t *n, size_t nlen,
                         const uint8_t *e, size_t elen, const char *issuer,
                         uint64_t now_sec, unsigned leeway_sec,
                         char *sub_out, size_t sub_len) {
    if (!token || !n || !e || !issuer || !issuer[0]) return -1;
    if (nlen < CMQ_JWT_RSA_N_MIN || nlen > CMQ_JWT_RSA_N_MAX) return -1;
    if (elen == 0 || elen > CMQ_JWT_RSA_E_MAX) return -1;
    size_t tlen = strnlen(token, CMQ_JWT_TOKEN_MAX + 1);
    if (tlen == 0 || tlen > CMQ_JWT_TOKEN_MAX) return -1;
    const char *d1 = strchr(token, '.');
    if (!d1) return -1;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2 || strchr(d2 + 1, '.')) return -1;
    size_t hlen = (size_t)(d1 - token);
    size_t plen = (size_t)(d2 - d1 - 1);
    size_t sigl = strlen(d2 + 1);
    if (hlen == 0 || plen == 0 || sigl == 0) return -1;

    uint8_t hdr[256], pay[1024], sig[CMQ_JWT_RSA_N_MAX];
    size_t hdr_n = 0, pay_n = 0, sig_n = 0;
    if (b64url_decode(token, hlen, hdr, sizeof(hdr) - 1, &hdr_n) != 0)
        return -1;
    if (b64url_decode(d1 + 1, plen, pay, sizeof(pay) - 1, &pay_n) != 0)
        return -1;
    if (b64url_decode(d2 + 1, sigl, sig, sizeof(sig), &sig_n) != 0)
        return -1;
    hdr[hdr_n] = '\0';
    pay[pay_n] = '\0';
    char alg[16] = {0};
    if (json_str_claim((char *)hdr, hdr_n, "alg", alg, sizeof(alg)) != 0)
        return -1;
    if (strcmp(alg, "RS256") != 0) return -1;
    if (sig_n < CMQ_JWT_RSA_N_MIN || sig_n > CMQ_JWT_RSA_N_MAX) return -1;

    BIGNUM *bn = BN_bin2bn(n, (int)nlen, NULL);
    BIGNUM *be = BN_bin2bn(e, (int)elen, NULL);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY *pkey = NULL;
    int rc = -1;
    if (bn && be && pctx && bld && EVP_PKEY_fromdata_init(pctx) == 1 &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be))
        params = OSSL_PARAM_BLD_to_param(bld);
    if (params &&
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1 &&
        pkey) {
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        size_t signing_len = hlen + 1 + plen;
        if (m &&
            EVP_DigestVerifyInit(m, NULL, EVP_sha256(), NULL, pkey) == 1 &&
            EVP_DigestVerify(m, sig, sig_n, (const unsigned char *)token,
                             signing_len) == 1)
            rc = jwt_check_claims((char *)pay, pay_n, issuer, now_sec,
                                  leeway_sec, sub_out, sub_len);
        EVP_MD_CTX_free(m);
    }
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    BN_free(bn);
    BN_free(be);
    return rc;
}

int cmq_jwks_parse(const char *json, cmq_jwks_t *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = strnlen(json, CMQ_JWKS_JSON_MAX + 1);
    if (n == 0 || n > CMQ_JWKS_JSON_MAX) return -1;
    const char *keys = strstr(json, "\"keys\"");
    if (!keys) return -1;
    const char *arr = strchr(keys, '[');
    if (!arr) return -1;
    const char *p = arr + 1;
    const char *end = json + n;
    while (p < end && out->n < CMQ_JWKS_MAX_KEYS) {
        const char *ob = strchr(p, '{');
        if (!ob || ob >= end) break;
        const char *oe = strchr(ob, '}');
        if (!oe || oe >= end) return -1;
        size_t on = (size_t)(oe - ob + 1);
        char kty[8] = {0}, alg[12] = {0}, kid[64] = {0};
        if (json_str_claim(ob, on, "kid", kid, sizeof(kid)) != 0 || !kid[0])
            return -1;
        if (json_str_claim(ob, on, "kty", kty, sizeof(kty)) != 0)
            return -1;
        for (int i = 0; i < out->n; i++) {
            if (strcmp(out->keys[i].kid, kid) == 0)
                return -1;
        }
        cmq_jwks_key_t *slot = &out->keys[out->n];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->kid, sizeof(slot->kid), "%s", kid);
        if (strcmp(kty, "EC") == 0) {
            char crv[12] = {0}, xs[64] = {0}, ys[64] = {0};
            if (json_str_claim(ob, on, "crv", crv, sizeof(crv)) != 0 ||
                strcmp(crv, "P-256") != 0)
                return -1;
            if (json_str_claim(ob, on, "x", xs, sizeof(xs)) != 0 ||
                json_str_claim(ob, on, "y", ys, sizeof(ys)) != 0)
                return -1;
            if (json_str_claim(ob, on, "alg", alg, sizeof(alg)) == 0 &&
                strcmp(alg, "ES256") != 0)
                return -1;
            size_t xn = 0, yn = 0;
            if (b64url_decode(xs, strlen(xs), slot->x, sizeof(slot->x), &xn)
                    != 0 ||
                xn != CMQ_JWT_EC_XY_LEN)
                return -1;
            if (b64url_decode(ys, strlen(ys), slot->y, sizeof(slot->y), &yn)
                    != 0 ||
                yn != CMQ_JWT_EC_XY_LEN)
                return -1;
            slot->kty = CMQ_JWKS_KTY_EC;
        } else if (strcmp(kty, "oct") == 0) {
            char k[172] = {0};
            if (json_str_claim(ob, on, "k", k, sizeof(k)) != 0 || !k[0])
                return -1;
            if (json_str_claim(ob, on, "alg", alg, sizeof(alg)) == 0 &&
                strcmp(alg, "HS256") != 0)
                return -1;
            size_t rn = 0;
            if (b64url_decode(k, strlen(k), slot->secret, sizeof(slot->secret),
                              &rn) != 0 ||
                rn == 0 || rn >= sizeof(slot->secret))
                return -1;
            slot->slen = rn;
            slot->kty = CMQ_JWKS_KTY_OCT;
        } else if (strcmp(kty, "RSA") == 0) {
            char ns[800] = {0}, es[16] = {0};
            if (json_str_claim(ob, on, "n", ns, sizeof(ns)) != 0 ||
                json_str_claim(ob, on, "e", es, sizeof(es)) != 0)
                return -1;
            if (json_str_claim(ob, on, "alg", alg, sizeof(alg)) == 0 &&
                strcmp(alg, "RS256") != 0)
                return -1;
            if (cmq_jwt_rsa_decode(ns, es, slot->n, &slot->nlen, slot->e,
                                   &slot->elen) != 0)
                return -1;
            slot->kty = CMQ_JWKS_KTY_RSA;
        } else {
            return -1;
        }
        out->n++;
        p = oe + 1;
    }
    if (out->n == 0) return -1;
    return 0;
}

int cmq_jwks_lookup(const cmq_jwks_t *j, const char *kid,
                    const uint8_t **secret, size_t *slen) {
    if (!j || !kid || !kid[0] || !secret || !slen) return -1;
    for (int i = 0; i < j->n; i++) {
        if (j->keys[i].kty == CMQ_JWKS_KTY_OCT &&
            strcmp(j->keys[i].kid, kid) == 0) {
            *secret = j->keys[i].secret;
            *slen = j->keys[i].slen;
            return 0;
        }
    }
    return -1;
}

int cmq_jwks_lookup_ec(const cmq_jwks_t *j, const char *kid,
                       const uint8_t **x, const uint8_t **y) {
    if (!j || !kid || !kid[0] || !x || !y) return -1;
    for (int i = 0; i < j->n; i++) {
        if (j->keys[i].kty == CMQ_JWKS_KTY_EC &&
            strcmp(j->keys[i].kid, kid) == 0) {
            *x = j->keys[i].x;
            *y = j->keys[i].y;
            return 0;
        }
    }
    return -1;
}

int cmq_jwks_lookup_rsa(const cmq_jwks_t *j, const char *kid,
                        const uint8_t **n, size_t *nlen,
                        const uint8_t **e, size_t *elen) {
    if (!j || !kid || !kid[0] || !n || !nlen || !e || !elen) return -1;
    for (int i = 0; i < j->n; i++) {
        if (j->keys[i].kty == CMQ_JWKS_KTY_RSA &&
            strcmp(j->keys[i].kid, kid) == 0) {
            *n = j->keys[i].n;
            *nlen = j->keys[i].nlen;
            *e = j->keys[i].e;
            *elen = j->keys[i].elen;
            return 0;
        }
    }
    return -1;
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

#define CMQ_NKEY_PFX_USER ((uint8_t)(20u << 3))
#define CMQ_NKEY_PFX_SEED ((uint8_t)(18u << 3))

static uint16_t nkey_crc16(const uint8_t *data, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u)
                      ? (uint16_t)((crc << 1) ^ 0x1021u)
                      : (uint16_t)(crc << 1);
    }
    return crc;
}

static int nkey_b32_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

static int nkey_b32_decode(const char *in, uint8_t *out, size_t cap,
                           size_t *out_len) {
    if (!in || !out || !out_len) return -1;
    size_t n = strnlen(in, 80);
    if (n < 8 || n > 64) return -1;
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = nkey_b32_val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 5) | (unsigned)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = o;
    return 0;
}

static int nkey_b32_encode(const uint8_t *in, size_t in_len, char *out,
                           size_t cap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    if (!in || !out || in_len == 0 || cap == 0) return -1;
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (o + 1 >= cap) return -1;
            out[o++] = t[(acc >> bits) & 31];
        }
    }
    if (bits > 0) {
        if (o + 1 >= cap) return -1;
        out[o++] = t[(acc << (5 - bits)) & 31];
    }
    out[o] = '\0';
    return 0;
}

int cmq_nkey_pub_encode(const uint8_t pub[CMQ_NKEY_PUB_LEN], char *out,
                        size_t cap) {
    if (!pub || !out) return -1;
    uint8_t raw[35];
    raw[0] = CMQ_NKEY_PFX_USER;
    memcpy(raw + 1, pub, CMQ_NKEY_PUB_LEN);
    uint16_t crc = nkey_crc16(raw, 33);
    raw[33] = (uint8_t)(crc & 0xff);
    raw[34] = (uint8_t)(crc >> 8);
    return nkey_b32_encode(raw, sizeof(raw), out, cap);
}

int cmq_nkey_pub_decode(const char *s, uint8_t pub[CMQ_NKEY_PUB_LEN]) {
    if (!s || !pub) return -1;
    size_t n = strnlen(s, 80);
    if (n == 64) return cmq_nkey_hex_decode(s, pub, CMQ_NKEY_PUB_LEN);
    uint8_t raw[40];
    size_t rn = 0;
    if (nkey_b32_decode(s, raw, sizeof(raw), &rn) != 0 || rn != 35)
        return -1;
    uint16_t got = (uint16_t)raw[33] | ((uint16_t)raw[34] << 8);
    if (nkey_crc16(raw, 33) != got) return -1;
    if (raw[0] != CMQ_NKEY_PFX_USER) return -1;
    memcpy(pub, raw + 1, CMQ_NKEY_PUB_LEN);
    return 0;
}

int cmq_nkey_seed_decode(const char *s, uint8_t seed[CMQ_NKEY_PUB_LEN]) {
    if (!s || !seed) return -1;
    uint8_t raw[40];
    size_t rn = 0;
    if (nkey_b32_decode(s, raw, sizeof(raw), &rn) != 0 || rn != 36)
        return -1;
    uint16_t got = (uint16_t)raw[34] | ((uint16_t)raw[35] << 8);
    if (nkey_crc16(raw, 34) != got) return -1;
    uint8_t b1 = (uint8_t)(raw[0] & 248u);
    uint8_t role = (uint8_t)(((raw[0] & 7u) << 5) | ((raw[1] & 248u) >> 3));
    if (b1 != CMQ_NKEY_PFX_SEED || role != CMQ_NKEY_PFX_USER) return -1;
    memcpy(seed, raw + 2, CMQ_NKEY_PUB_LEN);
    return 0;
}

int cmq_nkey_seed_to_pub(const uint8_t seed[CMQ_NKEY_PUB_LEN],
                         uint8_t pub[CMQ_NKEY_PUB_LEN]) {
    if (!seed || !pub) return -1;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed,
                                                  CMQ_NKEY_PUB_LEN);
    if (!pkey) return -1;
    size_t n = CMQ_NKEY_PUB_LEN;
    int rc = EVP_PKEY_get_raw_public_key(pkey, pub, &n) == 1 &&
                     n == CMQ_NKEY_PUB_LEN
                 ? 0
                 : -1;
    EVP_PKEY_free(pkey);
    return rc;
}
