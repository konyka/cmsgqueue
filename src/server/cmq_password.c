#define _POSIX_C_SOURCE 200809L
#include "cmq_password.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CMQ_SCRYPT_N      16384u
#define CMQ_SCRYPT_R      8u
#define CMQ_SCRYPT_P      1u
#define CMQ_SALT_LEN      16u
#define CMQ_HASH_LEN      32u
#define CMQ_MAXMEM        (64u * 1024u * 1024u)

/* Base64url encoding: always produces 4-aligned output. */
static int b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    /* Round up to 4-char blocks. */
    size_t need = 4 * ((in_len + 2) / 3);
    if (out_cap < need + 1) return -1;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = tab[(v >> 18) & 0x3F];
        out[o++] = tab[(v >> 12) & 0x3F];
        out[o++] = tab[(v >> 6) & 0x3F];
        out[o++] = tab[v & 0x3F];
        i += 3;
    }
    if (i < in_len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i+1] << 8;
        out[o++] = tab[(v >> 18) & 0x3F];
        out[o++] = tab[(v >> 12) & 0x3F];
        if (i + 1 < in_len) {
            out[o++] = tab[(v >> 6) & 0x3F];
            out[o++] = tab[v & 0x3F];
        }
    }
    /* Zero out unused tail chars (e.g., 16 bytes -> 22 chars, pad to 24
     * with zeros which are 'A' in the alphabet). We pick 'A' since
     * it's the natural "zero" in base64. The decoder ignores these. */
    while (o < need) out[o++] = 'A';
    out[o] = '\0';
    return (int)o;
}

static int b64url_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
    static const int8_t tab[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    size_t in_len = strlen(in);
    if (in_len == 0 || in_len % 4 != 0) return -1;
    /* base64url without padding: trailing 1-2 chars of the last
     * 4-char block may be absent. The encoder produces 0, 2, or 3
     * chars for the last block (no padding). The decoder detects the
     * actual length by counting non-'-_' chars in the last block. */
    size_t full_blocks = in_len / 4;
    if (full_blocks == 0) return -1;
    size_t tail_chars = in_len - (full_blocks - 1) * 4;
    size_t pad_equiv = 0;
    if (tail_chars == 2) pad_equiv = 2;
    else if (tail_chars == 3) pad_equiv = 1;
    /* else tail_chars == 4, no padding */
    size_t decoded = (full_blocks * 3) - pad_equiv;
    if (decoded > out_cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        if (in[i] >= 128 || tab[(int)in[i]] < 0) return -1;
        int v0 = tab[(int)in[i]];
        int v1 = (in[i+1] >= 128 || tab[(int)in[i+1]] < 0) ? 0 : tab[(int)in[i+1]];
        int v2 = (in[i+2] >= 128 || tab[(int)in[i+2]] < 0) ? 0 : tab[(int)in[i+2]];
        int v3 = (in[i+3] >= 128 || tab[(int)in[i+3]] < 0) ? 0 : tab[(int)in[i+3]];
        uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        if (o < decoded) out[o++] = (uint8_t)(v >> 16);
        if (o < decoded) out[o++] = (uint8_t)(v >> 8);
        if (o < decoded) out[o++] = (uint8_t)v;
    }
    *out_len = decoded;
    return 0;
}

int cmq_password_hash(const char *password, char *out, size_t out_len) {
    if (!password || !out) return -1;
    size_t pwlen = strlen(password);
    if (pwlen == 0) return -1;

    uint8_t salt[CMQ_SALT_LEN];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return -1;

    uint8_t hash[CMQ_HASH_LEN];
    if (EVP_PBE_scrypt(password, (size_t)pwlen, salt, sizeof(salt),
                        (uint64_t)CMQ_SCRYPT_N, (uint64_t)CMQ_SCRYPT_R,
                        (uint64_t)CMQ_SCRYPT_P, CMQ_MAXMEM,
                        hash, sizeof(hash)) != 1) {
        return -1;
    }

    char salt_b64[32];
    char hash_b64[64];
    if (b64url_encode(salt, sizeof(salt), salt_b64, sizeof(salt_b64)) < 0) return -1;
    if (b64url_encode(hash, sizeof(hash), hash_b64, sizeof(hash_b64)) < 0) return -1;

    /* Wire format: $scrypt$N=NNN,r=NN,p=NN,salt_len=NN,hash_len=NN$<salt>$<hash>
     * Explicit lengths so the decoder knows exact byte counts. */
    int n = snprintf(out, out_len, "$scrypt$N=%u,r=%u,p=%u,salt_len=%zu,hash_len=%zu$%s$%s",
                    CMQ_SCRYPT_N, CMQ_SCRYPT_R, CMQ_SCRYPT_P,
                    sizeof(salt), sizeof(hash),
                    salt_b64, hash_b64);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

int cmq_password_verify(const char *stored, const char *password) {
    if (!stored || !password) return -1;

    /* Legacy plaintext prefix. */
    const char *legacy = "$plaintext$";
    if (strncmp(stored, legacy, strlen(legacy)) == 0) {
        const char *leg = stored + strlen(legacy);
        return strcmp(leg, password) == 0 ? 1 : 0;
    }

    /* Scrypt format. */
    const char *prefix = "$scrypt$";
    if (strncmp(stored, prefix, strlen(prefix)) != 0) return -1;
    const char *pos = stored + strlen(prefix);

    /* Parse parameters: N=NNN,r=NN,p=NN,salt_len=NN,hash_len=NN$salt$hash */
    uint64_t N = 0, r = 0, p = 0;
    size_t salt_len = 0, hash_len = 0;
    char *end;
    if (strncmp(pos, "N=", 2) != 0) return -1;
    N = strtoull(pos + 2, &end, 10);
    if (*end != ',') return -1;
    if (strncmp(end, ",r=", 3) != 0) return -1;
    r = strtoull(end + 3, &end, 10);
    if (*end != ',') return -1;
    if (strncmp(end, ",p=", 3) != 0) return -1;
    end += 3;
    uint64_t pp = strtoull(end, &end, 10);
    if (*end != ',') return -1;
    if (strncmp(end, ",salt_len=", 10) != 0) return -1;
    salt_len = (size_t)strtoull(end + 10, &end, 10);
    if (*end != ',') return -1;
    if (strncmp(end, ",hash_len=", 10) != 0) return -1;
    hash_len = (size_t)strtoull(end + 10, &end, 10);
    if (*end != '$') return -1;
    const char *salt_b64 = end + 1;
    const char *dollar = strchr(salt_b64, '$');
    if (!dollar) return -1;

    size_t salt_b64_len = (size_t)(dollar - salt_b64);
    if (salt_b64_len == 0 || salt_b64_len >= 64) return -1;
    char salt_str[64];
    memcpy(salt_str, salt_b64, salt_b64_len);
    salt_str[salt_b64_len] = '\0';

    const char *hash_b64 = dollar + 1;
    uint8_t salt[64], hash[64];
    size_t dec_salt_len = 0, dec_hash_len = 0;
    if (b64url_decode(salt_str, salt, sizeof(salt), &dec_salt_len) != 0) return -1;
    if (b64url_decode(hash_b64, hash, sizeof(hash), &dec_hash_len) != 0) return -1;
    /* The encoded form is 4-aligned; the decoder includes 'A'-padded
     * tail chars. The declared salt_len / hash_len is the source of
     * truth. */
    if (dec_salt_len < salt_len || dec_hash_len < hash_len) return -1;

    uint8_t computed[64];
    if (hash_len > sizeof(computed)) return -1;
    if (EVP_PBE_scrypt(password, strlen(password), salt, salt_len,
                        N, r, pp, CMQ_MAXMEM,
                        computed, hash_len) != 1) {
        return -1;
    }
    /* Constant-time compare. */
    uint8_t diff = 0;
    for (size_t i = 0; i < hash_len; i++) diff |= (computed[i] ^ hash[i]);
    return diff == 0 ? 1 : 0;
}
