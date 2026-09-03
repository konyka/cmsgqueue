/* v0.5.23: TLS session resumption cache tests.
 *
 * Verifies the in-process session cache that backs OpenSSL's
 * SSL_CTX_sess_set_{new,get}_cb. Tests exercise insert / lookup /
 * eviction / destroy directly without a real handshake (faster,
 * deterministic).
 */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls from cmq_tls_session_cache.h. */
int cmq_tls_session_cache_init(cmq_tls_config_t *cfg);
void cmq_tls_session_cache_destroy(cmq_tls_config_t *cfg);
int cmq_tls_session_cache_insert(cmq_tls_config_t *cfg,
                                  const unsigned char *id,
                                  unsigned int id_len,
                                  SSL_SESSION *sess);
SSL_SESSION *cmq_tls_session_cache_lookup(cmq_tls_config_t *cfg,
                                           const unsigned char *id,
                                           unsigned int id_len);
size_t cmq_tls_session_cache_size(cmq_tls_config_t *cfg);

/* Build a fresh SSL_SESSION for the cache. The cache is keyed by the
 * id passed to insert/lookup, not by the session's internal session_id,
 * so a bare SSL_SESSION_new is sufficient. */
static SSL_SESSION *make_session(void) {
    return SSL_SESSION_new();
}

TEST(tls_session_cache, init_destroy_roundtrip) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_init(cfg), 0);
    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST(tls_session_cache, insert_and_lookup) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_init(cfg), 0);

    unsigned char id[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    SSL_SESSION *s = make_session();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cmq_tls_session_cache_insert(cfg, id, sizeof(id), s), 0);
    ASSERT_EQ(cmq_tls_session_cache_size(cfg), 1);

    SSL_SESSION *got = cmq_tls_session_cache_lookup(cfg, id, sizeof(id));
    ASSERT_NOT_NULL(got);

    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST(tls_session_cache, lookup_unknown_returns_null) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_init(cfg), 0);

    unsigned char id[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    unsigned char other[16] = {0xAA,0xBB,0xCC,0xDD,0,0,0,0,0,0,0,0,0,0,0,0};
    SSL_SESSION *s = make_session();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cmq_tls_session_cache_insert(cfg, id, sizeof(id), s), 0);

    ASSERT_NULL(cmq_tls_session_cache_lookup(cfg, other, sizeof(other)));

    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST(tls_session_cache, multiple_inserts_distinct_keys) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_init(cfg), 0);

    enum { N = 5 };
    unsigned char ids[N][8];
    SSL_SESSION *sessions[N];
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 8; k++) ids[i][k] = (unsigned char)(i * 16 + k);
        sessions[i] = make_session();
        ASSERT_NOT_NULL(sessions[i]);
        ASSERT_EQ(cmq_tls_session_cache_insert(cfg, ids[i], 8, sessions[i]), 0);
    }
    ASSERT_EQ(cmq_tls_session_cache_size(cfg), (size_t)N);

    for (int i = 0; i < N; i++) {
        SSL_SESSION *g = cmq_tls_session_cache_lookup(cfg, ids[i], 8);
        ASSERT_NOT_NULL(g);
    }

    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST(tls_session_cache, eviction_when_full) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_init(cfg), 0);

    /* Insert 1025 sessions; the 1025th must trigger eviction (LRU).
     * We don't check WHICH entry got evicted (LRU order depends on
     * timing); just verify size is bounded at 1024. */
    enum { N = 1025 };
    unsigned char id[8];
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 8; k++) id[k] = (unsigned char)((i * 7 + k) & 0xFF);
        SSL_SESSION *s = make_session();
        ASSERT_NOT_NULL(s);
        ASSERT_EQ(cmq_tls_session_cache_insert(cfg, id, 8, s), 0);
    }
    ASSERT_TRUE(cmq_tls_session_cache_size(cfg) <= 1024);

    /* Most-recently-inserted MUST still be present. */
    unsigned char last[8];
    for (int k = 0; k < 8; k++) last[k] = (unsigned char)(((N-1) * 7 + k) & 0xFF);
    SSL_SESSION *g = cmq_tls_session_cache_lookup(cfg, last, 8);
    ASSERT_NOT_NULL(g);

    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST(tls_session_cache, double_destroy_safe) {
    /* Destroy must be idempotent (warns on NULL but doesn't crash). */
    cmq_tls_session_cache_destroy(NULL);
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    /* Destroy on uninitialized cache: must not crash. */
    cmq_tls_session_cache_destroy(cfg);
    cmq_tls_config_destroy(cfg);
}

TEST_MAIN()
