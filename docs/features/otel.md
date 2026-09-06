# OpenTelemetry (v0.5.61–64, D1 phases 1–2)

`cmq_otel` queues spans on a 256-slot ring. A sidecar thread
drains them to an export hook. Offer never blocks.

## Span

Trace id (16 bytes), kind (`PUBLISH` / `CONSUME` / `CONNECT`),
timestamp. No subject, no payload.

## Overflow

When the ring is full, `offer` increments `dropped` and
returns 1 (newest span lost). Oldest in-flight spans stay.

## OTLP/HTTP JSON (v0.5.64)

When `otlp_endpoint` is set (`http://host[:port][/path]`),
the sidecar POSTs OTLP/JSON to that URL. Default port is
4318; default path is `/v1/traces`. HTTPS, gRPC, and
userinfo are rejected.

HTTP failures are ignored. The ring never waits on I/O.
No endpoint: the export hook stays NULL.

## Tests

`tests/test_otel.c`, `tests/test_otlp.c`

## See also

- `docs/reviews/v0.5.61.enumeration.md`
- `docs/reviews/v0.5.64.enumeration.md`
- `docs/features/tracing.md`
