# OpenTelemetry span ring (v0.5.61, D1 phase 1)

`cmq_otel` queues spans on a 256-slot ring. A sidecar thread
drains them to an export hook. Offer never blocks.

## Span

Trace id (16 bytes), kind (`PUBLISH` / `CONSUME` / `CONNECT`),
monotonic timestamp. No subject, no payload.

## Overflow

When the ring is full, `offer` increments `dropped` and
returns 1 (newest span lost). Oldest in-flight spans stay.

## Export

Default hook is empty; the sidecar still counts `exported`.
OTLP/gRPC is not in this increment.

The server offers a PUBLISH span after txn/idempo checks
and before WAL.

## Tests

`tests/test_otel.c`

## See also

- `docs/reviews/v0.5.61.enumeration.md`
- `docs/features/tracing.md`
