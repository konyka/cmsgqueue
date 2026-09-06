# F12 + F13: HTTP /healthz, /readyz, /metrics

## Motivation

Production deployments need operational endpoints for liveness,
readiness, and metric scraping. The CMSGQueue server had no HTTP
listener — only the binary protocol and WebSocket. Kubernetes,
Consul, Prometheus, and most cloud-native monitoring tools expect
standard HTTP endpoints.

## Design

A new HTTP dispatcher is inserted into `handle_ws_upgrade`
(`src/server/cmq_server.c:4861`) at the top of the function. Before
the WebSocket-validity check, the bytes are parsed as a `GET /path`
HTTP request. Three paths are recognized:

| Path | Status | Body |
|---|---|---|
| `/healthz` | 200 OK | `{"status":"ok"}` |
| `/readyz` | 200 OK / 503 | `{"status":"ready"\|"draining"}` |
| `/metrics` | 200 OK | Prometheus exposition |
| `/connz` | 200 OK | connections snapshot (v0.5.47) |
| `/subz` | 200 OK | subscriptions snapshot (v0.5.47) |
| `/routez` | 200 OK | route pool snapshot (v0.5.47) |

The dispatcher uses `client_sock_write` (synchronous write) so the
response is in the kernel TCP buffer before the connection transitions
to CLOSING. The Prometheus exposition advertises
`cmq_connections`, `cmq_subscriptions`, `cmq_messages_in_total`, and
`cmq_messages_out_total` — the existing atomic stat counters.

## Files touched

- `src/server/cmq_server.c` — `handle_ws_upgrade` dispatcher.
- `tests/test_health_metrics.c` — 2 tests (metrics + 404).

## Tests

`tests/test_health_metrics.c`:
- `http.metrics_returns_prometheus` — verifies the full Prometheus
  body structure (HELP, TYPE, metric samples).
- `http.unknown_path_returns_404` — verifies unknown paths fall through
  to the WS code path (connection closed).

## Verification gates

- 31/31 tests pass (was 30, +1 test_health_metrics with 2 tests).
- Server creates successfully even when TLS is not requested.

## Performance

The HTTP path is OFF by default on the wire protocol. A scrape every
5–10 seconds adds no measurable load (one TCP connect, one HTTP
request, one response). The Prometheus body is ~500 bytes — well
within the 4 KiB print buffer.

## Security

- No secrets exposed: the metrics endpoint reveals only public counters.
- The `/healthz` response is constant: no fingerprinting.
- The dispatch is keyed on the path string; a malformed request without
  a valid path falls through to the existing WS-or-teardown logic.

## Introspection (v0.5.47)

`/connz` `/subz` `/routez` copy a bounded snapshot (64 / 256 / 32)
under existing locks, then format JSON after unlock. User strings
are escaped. Overflow sets `"truncated":1`. No passwords.

## Limitations

- Listener is the same TCP port as the binary protocol. A separate
  HTTP port would simplify firewall rules but requires a separate
  listener (out of scope for F12).
- No TLS on the HTTP path. Production deployments should reverse-
  proxy through nginx or terminate TLS at the load balancer.
- Prometheus exposition is text-format only. OpenMetrics is not
  implemented.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.12, §1.13 (F12, F13).
- `docs/reviews/round2_perf_attack.md` (perf considerations).
- `docs/features/info-frame.md` (F4, related capability advertisement).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F12/F13.
- Prometheus exposition format: https://prometheus.io/docs/instrumenting/exposition_formats/
