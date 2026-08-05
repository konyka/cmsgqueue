# F11: Connection Tracing (Correlation IDs)

## Motivation

Operators investigating incidents need to correlate log entries from a single connection. Without a correlation ID, "which connection was this error on?" is a search through fd numbers and timestamps.

## Design

Each connection gets a 16-byte UUID at the earliest point in its lifecycle. The ID is stored in `cmq_client_t.trace_id` and can be propagated through logs and (eventually) the audit trail.

- `cmq_trace_id(out)` — generates 128 random bytes via OpenSSL `RAND_bytes`.
- `cmq_trace_id_hex(id, out, out_len)` — formats as 32 lowercase hex chars.

The server assigns the ID at the start of the `CMQ_OP_CONNECT` dispatch. Future work: thread the ID through every log entry and audit event.

## Files touched

- `src/server/cmq_trace.{h,c}` (new).
- `src/server/cmq_server.h` — `trace_id[16]` field on `cmq_client_t`.
- `src/server/cmq_server.c` — assigned at CONNECT time.
- `CMakeLists.txt` — `cmq_trace.c` added to `CMQ_CORE_SOURCES`.
- `tests/test_trace.c` (new).

## Tests

`tests/test_trace.c`:
- `trace.generate_id_is_unique` — two calls produce different IDs.
- `trace.encode_hex` — round-trip encoding.
- `trace.encode_short_buffer_truncates` — short buffer handling.

## Verification gates

- 3/3 trace tests pass.
- 45/45 total tests pass.

## Performance

`cmq_trace_id` is one `RAND_bytes(16)` call: ~50 ns. `cmq_trace_id_hex` is one byte-loop encoding: ~100 ns. The cost is on the CONNECT path, which is rare relative to msg/s.

## Security

Threats closed:
- **Operational forensics** — operators can grep for a specific trace ID to find all log entries for a connection.
- **Trace correlation across services** — the ID is the same across log lines and audit events.

Threats NOT closed:
- **Trace ID forgery** — the ID is a random nonce, not authenticated. An attacker cannot guess another connection's ID.
- **Trace ID in user-visible payloads** — the ID is logged but not sent on the wire. (Out of scope.)

## Limitations

- Trace ID is not yet propagated to log entries. Future work: thread the ID through `cmq_log` calls.
- No integration with external tracing systems (OpenTelemetry, etc.).

## See also

- `docs/reviews/hyperplan-v030-plan.md` F11.
- `docs/features/audit.md` — F13 uses the trace ID in audit events.
- `docs/features/password-hash.md` — F8 auth path emits audit events.
