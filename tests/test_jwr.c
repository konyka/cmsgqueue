/* v0.5.120: reload rebuilds the live JWKS cache from jwks_json. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
#include <stdlib.h>
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

static void oct_json(char *out, size_t cap, const char *kid, const char *sec) {
    char k[64];
    b64url_encode((const uint8_t *)sec, strlen(sec), k, sizeof(k));
    snprintf(out, cap,
             "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"%s\",\"k\":\"%s\"}]}",
             kid, k);
}

TEST(jwr, apply) {
    char oldj[256], newj[256];
    oct_json(oldj, sizeof(oldj), "k1", "s3cret");
    oct_json(newj, sizeof(newj), "k2", "newsec");
    cmq_jwks_t parsed;
    ASSERT_EQ(cmq_jwks_parse(oldj, &parsed), 0);
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT(c != NULL);
    ASSERT_EQ(cmq_jwks_cache_put(c, &parsed), 0);
    const char *live = strdup(oldj);
    ASSERT_EQ(cmq_jwks_cache_reload(&c, &live, newj), 0);
    ASSERT_STR_EQ(live, newj);
    const cmq_jwks_t *got = cmq_jwks_cache_get(c);
    ASSERT(got != NULL);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(got, "k2", &sec, &slen), 0);
    ASSERT_EQ(slen, (size_t)6);
    ASSERT(memcmp(sec, "newsec", 6) == 0);
    ASSERT(cmq_jwks_lookup(got, "k1", &sec, &slen) != 0);
    free((void *)live);
    cmq_jwks_cache_destroy(c);
}

TEST(jwr, omitted) {
    char oldj[256];
    oct_json(oldj, sizeof(oldj), "k1", "s3cret");
    cmq_jwks_t parsed;
    ASSERT_EQ(cmq_jwks_parse(oldj, &parsed), 0);
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT_EQ(cmq_jwks_cache_put(c, &parsed), 0);
    const char *live = strdup(oldj);
    ASSERT_EQ(cmq_jwks_cache_reload(&c, &live, NULL), 0);
    const cmq_jwks_t *got = cmq_jwks_cache_get(c);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(got, "k1", &sec, &slen), 0);
    ASSERT_STR_EQ(live, oldj);
    free((void *)live);
    cmq_jwks_cache_destroy(c);
}

TEST(jwr, empty) {
    cmq_jwks_cache_t *c = NULL;
    const char *live = NULL;
    ASSERT_EQ(cmq_jwks_cache_reload(&c, &live, ""), 0);
    ASSERT(c == NULL);
    ASSERT(live == NULL);
}

TEST(jwr, reject) {
    char oldj[256];
    oct_json(oldj, sizeof(oldj), "k1", "s3cret");
    cmq_jwks_t parsed;
    ASSERT_EQ(cmq_jwks_parse(oldj, &parsed), 0);
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT_EQ(cmq_jwks_cache_put(c, &parsed), 0);
    const char *live = strdup(oldj);
    ASSERT(cmq_jwks_cache_reload(&c, &live, "not-json") != 0);
    ASSERT_STR_EQ(live, oldj);
    const cmq_jwks_t *got = cmq_jwks_cache_get(c);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(got, "k1", &sec, &slen), 0);
    ASSERT(cmq_jwks_cache_reload(NULL, &live, oldj) != 0);
    free((void *)live);
    cmq_jwks_cache_destroy(c);
}

TEST_MAIN()
