# OpenTelemetry (v0.5.61–64, 0.5.78, 0.5.84, 0.5.89, 0.5.92, 0.5.100–102, D1)

`cmq_otel` queues spans on a 256-slot ring. A sidecar thread
drains them to an export hook. Offer never blocks.

## Span

Trace id (16 bytes), kind (`PUBLISH` / `CONSUME` / `CONNECT` /
`REQUEST` / `RESPONSE` / `DISCONNECT`), timestamp. No subject,
no payload.

`handle_publish` offers `PUBLISH` on the ingress path. After a
successful local fanout it offers one `CONSUME` with the same
trace (`cmq_otel_on_consume`). Zero subscribers and failed
deliver do not queue a consume span. CONNACK 0 offers one
`CONNECT` (`cmq_otel_on_connect`). CONNACK != 0 skips it.
A successful local REQUEST answer (store reply or fanout
`n > 0`) offers one `REQUEST` (`cmq_otel_on_request`). No
responders and route-only forward skip it. A successful
local RESPONSE deliver offers one `RESPONSE`
(`cmq_otel_on_response`). Failed deliver and route-only
forward skip it. A graceful inbound DISCONNECT offers one
`DISCONNECT` (`cmq_otel_on_disconnect`) before
`client_force_closing`.

## Overflow

When the ring is full, `offer` increments `dropped` and
returns 1 (newest span lost). Oldest in-flight spans stay.

## OTLP/HTTP JSON (v0.5.64 / v0.5.78)

When `otlp_endpoint` is set (`http(s)://host[:port][/path]`),
the sidecar POSTs OTLP/JSON to that URL. Default port is
4318; default path is `/v1/traces`. userinfo is rejected.

`grpc://` (v0.5.84) POSTs an OTLP protobuf Export over
prior-knowledge HTTP/2 (default port 4317). The sidecar
picks JSON or gRPC from the URL scheme.

`https://` uses TLS with peer verify. `otlp_ca` is an
optional PEM; otherwise the system CA store is used.
HTTP endpoints skip the handshake.
v0.5.135: reload applies a non-empty `otlp_ca` to the live
exporter URL. Omitted / empty keeps the current path.
`..` fails closed.
v0.5.138: reload applies a non-empty `otlp_endpoint` to the
live exporter (host/path/port/tls/grpc). Omitted / empty
keeps the current URL. A bad URL fails closed. CA is
preserved. Does not POST.
v0.5.139: reload attaches an exporter when create had none
(`cmq_otlp_reload_attach` + `cmq_otel_set_export`). Existing
exporters are left to v0.5.138. Does not POST.

HTTP/TLS failures are ignored. The ring never waits on I/O.
No endpoint: the export hook stays NULL.

## Tests

`tests/test_otel.c`, `tests/test_otlp.c`, `tests/test_otlps.c`,
`tests/test_otlpg.c`, `tests/test_otc.c`, `tests/test_otn.c`,
`tests/test_otr.c`, `tests/test_ots.c`, `tests/test_otd.c`,
`tests/test_oca.c`, `tests/test_oeu.c`, `tests/test_ota.c`

## See also

- `docs/reviews/v0.5.61.enumeration.md`
- `docs/reviews/v0.5.64.enumeration.md`
- `docs/reviews/v0.5.78.enumeration.md`
- `docs/reviews/v0.5.84.enumeration.md`
- `docs/reviews/v0.5.89.enumeration.md`
- `docs/reviews/v0.5.92.enumeration.md`
- `docs/reviews/v0.5.100.enumeration.md`
- `docs/reviews/v0.5.101.enumeration.md`
- `docs/reviews/v0.5.102.enumeration.md`
- `docs/features/tracing.md`
