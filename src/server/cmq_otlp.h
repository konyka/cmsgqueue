#ifndef CMQ_OTLP_H
#define CMQ_OTLP_H

#include "cmq_otel.h"
#include <stddef.h>

#define CMQ_OTLP_URL_MAX      256
#define CMQ_OTLP_JSON_MAX     4096
#define CMQ_OTLP_DEFAULT_PORT 4318
#define CMQ_OTLP_IO_MS        200
#define CMQ_OTLP_CA_MAX       256

typedef struct {
    char host[128];
    char path[128];
    int port;
    int tls;                 /* v0.5.78: 1 = https */
    char ca[CMQ_OTLP_CA_MAX];
} cmq_otlp_url_t;

#ifdef __cplusplus
extern "C" {
#endif

int cmq_otlp_parse_url(const char *url, cmq_otlp_url_t *out);
int cmq_otlp_set_ca(cmq_otlp_url_t *url, const char *ca_path);
/* Bytes written (no NUL); -1 on error. */
int cmq_otlp_encode_json(const cmq_otel_span_t *spans, size_t n,
                         char *out, size_t out_len);
int cmq_otlp_build_request(const cmq_otlp_url_t *url, const char *body,
                           char *out, size_t out_len);
/* 0 if HTTP 2xx; -1 on fail. Never blocks the offer path. */
int cmq_otlp_http_post(const cmq_otlp_url_t *url, const char *body);
void cmq_otlp_export(void *ctx, const cmq_otel_span_t *span);

#ifdef __cplusplus
}
#endif

#endif
