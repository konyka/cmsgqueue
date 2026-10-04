#ifndef CMQ_JWKSF_H
#define CMQ_JWKSF_H

#include "cmq_jwt.h"
#include <stddef.h>
#include <stdint.h>

#define CMQ_JWKS_URL_MAX      256
#define CMQ_JWKS_DEFAULT_PORT 80
#define CMQ_JWKS_TLS_PORT     443
#define CMQ_JWKS_IO_MS        200
#define CMQ_JWKS_CA_MAX       256

typedef struct {
    char host[128];
    char path[128];
    int port;
    int tls;                 /* v0.5.79: 1 = https */
    char ca[CMQ_JWKS_CA_MAX];
} cmq_jwks_url_t;

#ifdef __cplusplus
extern "C" {
#endif

int cmq_jwks_parse_url(const char *url, cmq_jwks_url_t *out);
int cmq_jwks_set_ca(cmq_jwks_url_t *url, const char *ca_path);
int cmq_jwks_build_get(const cmq_jwks_url_t *url, char *out, size_t cap);
/* GET + parse into out. 0 ok; -1 fail. */
int cmq_jwks_http_get(const cmq_jwks_url_t *url, cmq_jwks_t *out);

#define CMQ_JWKS_REFRESH_MIN 5
#define CMQ_JWKS_REFRESH_MAX 86400

typedef struct cmq_jwks_cache cmq_jwks_cache_t;
typedef struct cmq_jwks_refresher cmq_jwks_refresher_t;

cmq_jwks_cache_t *cmq_jwks_cache_create(void);
void cmq_jwks_cache_destroy(cmq_jwks_cache_t *c);
int cmq_jwks_cache_put(cmq_jwks_cache_t *c, const cmq_jwks_t *src);
const cmq_jwks_t *cmq_jwks_cache_get(const cmq_jwks_cache_t *c);
/* v0.5.120: parse fresh JWKS JSON into the live cache.
 * Empty/NULL json keeps the current cache. Bad JSON fails closed.
 * live_json (optional) takes ownership of a strdup of the document. */
int cmq_jwks_cache_reload(cmq_jwks_cache_t **cache, const char **live_json,
                          const char *fresh_json);

int cmq_jwks_refresh_due(uint64_t last_ms, uint64_t now_ms,
                         unsigned interval_sec);
int cmq_jwks_refresh_step(const cmq_jwks_url_t *url, cmq_jwks_cache_t *cache,
                          uint64_t *last_ms, uint64_t now_ms,
                          unsigned interval_sec);

cmq_jwks_refresher_t *cmq_jwks_refresh_start(const cmq_jwks_url_t *url,
                                             cmq_jwks_cache_t *cache,
                                             unsigned interval_sec);
void cmq_jwks_refresh_stop(cmq_jwks_refresher_t *r);
unsigned cmq_jwks_refresh_interval(cmq_jwks_refresher_t *r);
/* v0.5.131: 0 / omitted keeps. 5–86400 applies. URL fetch
 * stays create-time. */
int cmq_jwks_refresh_reload(cmq_jwks_refresher_t *r, int *live_sec,
                            int fresh_sec);
int cmq_jwks_refresh_ca(cmq_jwks_refresher_t *r, char *out, size_t cap);
/* v0.5.134: empty/omitted keeps. Invalid path fails closed.
 * Does not re-GET jwks_url. */
int cmq_jwks_refresh_reload_ca(cmq_jwks_refresher_t *r, const char **live_ca,
                               const char *fresh_ca);
/* v0.5.137: empty/omitted keeps. Bad URL fails closed.
 * Copies host/path/port/tls onto a live sidecar; preserves CA.
 * Does not re-GET. */
int cmq_jwks_refresh_reload_url(cmq_jwks_refresher_t *r, const char **live_url,
                                const char *fresh_url);
int cmq_jwks_refresh_snapshot(cmq_jwks_refresher_t *r, cmq_jwks_url_t *out);
/* v0.5.141: 0 / omitted / empty URL keeps off. Bad URL or
 * interval fails closed. Starts a sidecar when *r is NULL
 * and cache is live. Existing sidecar is left alone.
 * Does not GET. */
int cmq_jwks_refresh_attach(cmq_jwks_refresher_t **r, cmq_jwks_cache_t *cache,
                            const char *url, const char *ca, int fresh_sec);
/* v0.5.145: empty/omitted keeps off. Bad URL / GET fail closed.
 * First GET into a new cache when *cache is NULL. Existing
 * cache is left alone. Does not start the refresher. */
int cmq_jwks_reload_fetch(cmq_jwks_cache_t **cache, const char *url,
                          const char *ca);

#ifdef __cplusplus
}
#endif

#endif
