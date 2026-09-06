#ifndef CMQ_JWKSF_H
#define CMQ_JWKSF_H

#include "cmq_jwt.h"
#include <stddef.h>

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

#ifdef __cplusplus
}
#endif

#endif
