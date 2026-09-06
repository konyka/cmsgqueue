#ifndef CMQ_OTEL_H
#define CMQ_OTEL_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_OTEL_RING 256
#define CMQ_OTEL_KIND_PUBLISH 1
#define CMQ_OTEL_KIND_CONSUME 2
#define CMQ_OTEL_KIND_CONNECT 3
#define CMQ_OTEL_KIND_REQUEST 4
#define CMQ_OTEL_KIND_RESPONSE 5

typedef struct {
    uint8_t trace[16];
    uint8_t kind;
    uint64_t t_ms;
} cmq_otel_span_t;

typedef void (*cmq_otel_export_fn)(void *ctx, const cmq_otel_span_t *span);

typedef struct cmq_otel cmq_otel_t;

cmq_otel_t *cmq_otel_create(void);
void cmq_otel_destroy(cmq_otel_t *o);

/* 0 enqueued; 1 dropped (full); -1 bad args. Never blocks. */
int cmq_otel_offer(cmq_otel_t *o, const uint8_t trace[16], uint8_t kind);
/* v0.5.89: one CONSUME after successful local fanout.
 * delivered==0: no-op (0). Missing o/trace: -1. */
int cmq_otel_on_consume(cmq_otel_t *o, const uint8_t trace[16], int delivered);
/* v0.5.92: one CONNECT after CONNACK 0. ok==0: no-op (0). */
int cmq_otel_on_connect(cmq_otel_t *o, const uint8_t trace[16], int ok);
/* v0.5.100: one REQUEST after a successful local answer.
 * answered==0: no-op (0). Missing o/trace: -1. */
int cmq_otel_on_request(cmq_otel_t *o, const uint8_t trace[16], int answered);
/* v0.5.101: one RESPONSE after successful local deliver.
 * delivered==0: no-op (0). Missing o/trace: -1. */
int cmq_otel_on_response(cmq_otel_t *o, const uint8_t trace[16], int delivered);
/* 1 copied; 0 empty. */
int cmq_otel_poll(cmq_otel_t *o, cmq_otel_span_t *out);
uint64_t cmq_otel_dropped(const cmq_otel_t *o);
uint64_t cmq_otel_exported(const cmq_otel_t *o);

void cmq_otel_set_export(cmq_otel_t *o, cmq_otel_export_fn fn, void *ctx);
int cmq_otel_start(cmq_otel_t *o);
void cmq_otel_stop(cmq_otel_t *o);

#endif
