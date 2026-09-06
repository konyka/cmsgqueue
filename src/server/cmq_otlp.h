#ifndef CMQ_OTLP_H
#define CMQ_OTLP_H

#include "cmq_otel.h"
#include <stddef.h>

#define CMQ_OTLP_URL_MAX      256
#define CMQ_OTLP_JSON_MAX     4096
#define CMQ_OTLP_DEFAULT_PORT 4318
#define CMQ_OTLP_GRPC_PORT    4317
#define CMQ_OTLP_IO_MS        200
#define CMQ_OTLP_CA_MAX       256
#define CMQ_OTLP_GRPC_PATH \
    "/opentelemetry.proto.collector.trace.v1.TraceService/Export"

typedef struct {
    char host[128];
    char path[128];
    int port;
    int tls;                 /* v0.5.78: 1 = https */
    int grpc;                /* v0.5.84: 1 = grpc:// HTTP/2 */
    char ca[CMQ_OTLP_CA_MAX];
} cmq_otlp_url_t;

#ifdef __cplusplus
extern "C" {
#endif

int cmq_otlp_parse_url(const char *url, cmq_otlp_url_t *out);
int cmq_otlp_set_ca(cmq_otlp_url_t *url, const char *ca_path);
/* v0.5.135: empty/omitted keeps. Invalid path fails closed. */
int cmq_otlp_reload_ca(cmq_otlp_url_t *url, const char **live_ca,
                       const char *fresh_ca);
/* v0.5.138: empty/omitted keeps. Bad URL fails closed.
 * Copies host/path/port/tls/grpc onto a live exporter; preserves CA.
 * Does not POST. Exporter start stays create-time. */
int cmq_otlp_reload_url(cmq_otlp_url_t *url, const char **live_ep,
                        const char *fresh_ep);
/* v0.5.139: empty/omitted keeps. Bad URL fails closed.
 * Allocates a live exporter when *url is NULL. Existing
 * exporter is left alone (reload_url already applied).
 * Does not POST. */
int cmq_otlp_reload_attach(cmq_otlp_url_t **url, const char *fresh_ep);
/* Bytes written (no NUL); -1 on error. */
int cmq_otlp_encode_json(const cmq_otel_span_t *spans, size_t n,
                         char *out, size_t out_len);
int cmq_otlp_encode_proto(const cmq_otel_span_t *spans, size_t n,
                          uint8_t *out, size_t out_len);
int cmq_otlp_grpc_frame(const uint8_t *proto, size_t plen,
                        uint8_t *out, size_t out_len);
int cmq_otlp_build_request(const cmq_otlp_url_t *url, const char *body,
                           char *out, size_t out_len);
/* 0 if HTTP 2xx; -1 on fail. Never blocks the offer path. */
int cmq_otlp_http_post(const cmq_otlp_url_t *url, const char *body);
int cmq_otlp_grpc_post(const cmq_otlp_url_t *url, const cmq_otel_span_t *span);
void cmq_otlp_export(void *ctx, const cmq_otel_span_t *span);

#ifdef __cplusplus
}
#endif

#endif
