/* v0.5.23: TLS session resumption cache.
 *
 * Bounded LRU map from session-id to SSL_SESSION*. Used as the backing
 * store for OpenSSL's SSL_CTX_sess_set_new_cb / SSL_CTX_sess_set_get_cb.
 *
 * Why custom (vs OpenSSL's internal cache):
 *   - Inspectable from tests (OpenSSL's internal cache is opaque).
 *   - Bounded memory at exactly CMQ_TLS_SESSION_CACHE_MAX entries.
 *   - Stable identity across OpenSSL versions (the internal API has
 *     churned across 1.0 / 1.1 / 3.x).
 */

#define _POSIX_C_SOURCE 200809L
#include "cmq_tls_session_cache.h"
#include "cmq_tls.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#include <pthread.h>

struct cmq_tls_session_cache_entry {
    unsigned char id[CMQ_TLS_SESSION_ID_MAX];
    unsigned int id_len;
    void *sess;
    uint64_t last_used_ms;
    int occupied;
};

typedef struct cmq_tls_session_cache_entry cmq_tls_session_cache_entry_t;

struct cmq_tls_session_cache {
    struct cmq_tls_session_cache_entry buckets[CMQ_TLS_SESSION_CACHE_BUCKETS];
    size_t count;
    int initialized;
    pthread_mutex_t lock;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static unsigned int cache_hash(const unsigned char *id, unsigned int id_len) {
    /* FNV-1a 32-bit. Cheap, good distribution for our short keys. */
    unsigned int h = 2166136261u;
    for (unsigned int i = 0; i < id_len; i++) {
        h ^= id[i];
        h *= 16777619u;
    }
    return h;
}

static void cache_entry_free(cmq_tls_session_cache_entry_t *e) {
    if (!e || !e->occupied || !e->sess) return;
    cmq_tls_session_free_slot(e->sess);
    e->sess = NULL;
    e->occupied = 0;
    e->id_len = 0;
}

int cmq_tls_session_cache_init(cmq_tls_config_t *cfg) {
    if (!cfg) return -1;
    cmq_tls_session_cache_t *cache =
        (cmq_tls_session_cache_t *)calloc(1, sizeof(*cache));
    if (!cache) return -1;
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        free(cache);
        return -1;
    }
    cache->initialized = 1;
    if (cmq_tls_set_session_cache_state(cfg, cache) != 0) {
        pthread_mutex_destroy(&cache->lock);
        free(cache);
        return -1;
    }
    return 0;
}

void cmq_tls_session_cache_destroy(cmq_tls_config_t *cfg) {
    if (!cfg) return;
    cmq_tls_session_cache_t *cache =
        (cmq_tls_session_cache_t *)cmq_tls_get_session_cache_state(cfg);
    if (!cache) return;
    if (cache->initialized) {
        pthread_mutex_lock(&cache->lock);
        for (size_t i = 0; i < CMQ_TLS_SESSION_CACHE_BUCKETS; i++)
            cache_entry_free(&cache->buckets[i]);
        cache->initialized = 0;
        pthread_mutex_unlock(&cache->lock);
        pthread_mutex_destroy(&cache->lock);
    }
    free(cache);
    cmq_tls_set_session_cache_state(cfg, NULL);
}

int cmq_tls_session_cache_insert(cmq_tls_config_t *cfg,
                                  const unsigned char *id,
                                  unsigned int id_len,
                                  void *sess) {
    if (!cfg || !id || id_len == 0 || id_len > CMQ_TLS_SESSION_ID_MAX)
        return -1;
    if (!sess) return -1;

    cmq_tls_session_cache_t *cache =
        (cmq_tls_session_cache_t *)cmq_tls_get_session_cache_state(cfg);
    if (!cache || !cache->initialized) return -1;

    pthread_mutex_lock(&cache->lock);

    unsigned int h = cache_hash(id, id_len);
    unsigned int idx = h % CMQ_TLS_SESSION_CACHE_BUCKETS;

    int evicted = 0;
    for (size_t step = 0; step < CMQ_TLS_SESSION_CACHE_BUCKETS; step++) {
        cmq_tls_session_cache_entry_t *e = &cache->buckets[(idx + step) %
            CMQ_TLS_SESSION_CACHE_BUCKETS];
        if (!e->occupied) {
            memcpy(e->id, id, id_len);
            e->id_len = id_len;
            e->sess = sess;
            e->occupied = 1;
            e->last_used_ms = now_ms();
            cache->count++;
            evicted = 1;
            break;
        }
        if (e->id_len == id_len && memcmp(e->id, id, id_len) == 0) {
            cmq_tls_session_free_slot(e->sess);
            e->sess = sess;
            e->last_used_ms = now_ms();
            evicted = 1;
            break;
        }
    }

    if (!evicted) {
        /* Cache full — find LRU slot and evict. */
        size_t oldest = 0;
        uint64_t oldest_ts = UINT64_MAX;
        for (size_t i = 0; i < CMQ_TLS_SESSION_CACHE_BUCKETS; i++) {
            if (cache->buckets[i].last_used_ms < oldest_ts) {
                oldest_ts = cache->buckets[i].last_used_ms;
                oldest = i;
            }
        }
        cmq_tls_session_cache_entry_t *e = &cache->buckets[oldest];
        cmq_tls_session_free_slot(e->sess);
        memcpy(e->id, id, id_len);
        e->id_len = id_len;
        e->sess = sess;
        e->occupied = 1;
        e->last_used_ms = now_ms();
    }

    pthread_mutex_unlock(&cache->lock);
    return 0;
}

void *cmq_tls_session_cache_lookup(cmq_tls_config_t *cfg,
                                    const unsigned char *id,
                                    unsigned int id_len) {
    if (!cfg || !id || id_len == 0 || id_len > CMQ_TLS_SESSION_ID_MAX)
        return NULL;

    cmq_tls_session_cache_t *cache =
        (cmq_tls_session_cache_t *)cmq_tls_get_session_cache_state(cfg);
    if (!cache || !cache->initialized) return NULL;

    pthread_mutex_lock(&cache->lock);
    unsigned int h = cache_hash(id, id_len);
    unsigned int idx = h % CMQ_TLS_SESSION_CACHE_BUCKETS;
    void *found = NULL;
    for (size_t step = 0; step < CMQ_TLS_SESSION_CACHE_BUCKETS; step++) {
        cmq_tls_session_cache_entry_t *e = &cache->buckets[(idx + step) %
            CMQ_TLS_SESSION_CACHE_BUCKETS];
        if (!e->occupied) break;
        if (e->id_len == id_len && memcmp(e->id, id, id_len) == 0) {
            e->last_used_ms = now_ms();
            found = e->sess;
            break;
        }
    }
    pthread_mutex_unlock(&cache->lock);
    return found;
}

size_t cmq_tls_session_cache_size(cmq_tls_config_t *cfg) {
    if (!cfg) return 0;
    cmq_tls_session_cache_t *cache =
        (cmq_tls_session_cache_t *)cmq_tls_get_session_cache_state(cfg);
    if (!cache) return 0;
    pthread_mutex_lock(&cache->lock);
    size_t n = cache->count;
    pthread_mutex_unlock(&cache->lock);
    return n;
}
