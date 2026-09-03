#ifndef CMQ_TLS_SESSION_CACHE_H
#define CMQ_TLS_SESSION_CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmq_tls_config cmq_tls_config_t;

#define CMQ_TLS_SESSION_CACHE_BUCKETS 64
#define CMQ_TLS_SESSION_CACHE_MAX     1024
#define CMQ_TLS_SESSION_ID_MAX        32

/* Forward-declared; layout private to cmq_tls_session_cache.c. */
typedef struct cmq_tls_session_cache cmq_tls_session_cache_t;

int cmq_tls_session_cache_init(cmq_tls_config_t *cfg);
void cmq_tls_session_cache_destroy(cmq_tls_config_t *cfg);

int cmq_tls_session_cache_insert(cmq_tls_config_t *cfg,
                                  const unsigned char *id,
                                  unsigned int id_len,
                                  void *sess);

/* Returns a borrowed reference (matches OpenSSL's SSL_get_session).
 * The caller must NOT SSL_SESSION_free the result. */
void *cmq_tls_session_cache_lookup(cmq_tls_config_t *cfg,
                                    const unsigned char *id,
                                    unsigned int id_len);
size_t cmq_tls_session_cache_size(cmq_tls_config_t *cfg);

#endif
