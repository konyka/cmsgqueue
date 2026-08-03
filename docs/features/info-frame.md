# F4: Extended INFO Frame with Capabilities

## Motivation

`CMQ_OP_INFO` is sent on every connection to advertise the server's
capabilities and current state. Prior to F4, the JSON payload only
included `{version, proto, connections, subscriptions, auth}` — not
enough for clients to negotiate wire features (compression, checksum,
TLS) before sending data.

For F3 (wire checksum) and the upcoming F2 (compression) / F1 (TLS),
clients need to know **what the server supports** before setting
flags. NATS Server's INFO frame is the canonical pattern: advertise
the supported codecs, the TLS posture, the max payload, and the
server identity.

## Design

The `send_info_frame` function (`src/server/cmq_server.c:4726`) now
emits the following JSON shape on every connection:

```json
{
  "server_id": "cmsgsrv",
  "version": "0.2.0",
  "proto": 1,
  "go": "20m",
  "host": "0.0.0.0",
  "port": 7654,
  "max_payload": 1048576,
  "connections": 0,
  "subscriptions": 0,
  "auth": false,
  "tls": false,
  "compression": "none",
  "checksum": "crc32c",
  "headers": true,
  "batch": true
}
```

The forward-compatibility plan is:

| Field | Value today | After F1 | After F2 |
|---|---|---|---|
| `tls` | `false` | `true` (when configured) | unchanged |
| `compression` | `"none"` | `"none"` | `"zstd"` |
| `checksum` | `"crc32c"` | unchanged | unchanged |
| `headers` | `true` | unchanged | unchanged |
| `batch` | `true` | unchanged | unchanged |

This is a wire-format **additive** change: existing clients that
ignore the new fields continue to work. New clients can detect
`"checksum": "crc32c"` and use `CMQ_FLAG_CHECKSUM` on subsequent
PUBLISHes.

## Files touched

- `src/server/cmq_server.c` — `send_info_frame` updated.
- `tests/test_info.c` — 3 new tests verifying the JSON shape.

## Tests

`tests/test_info.c`:
- `info.expected_capability_keys` — verifies all 13 expected keys are
  present in the JSON.
- `info.compression_is_none_until_f2` — verifies the value is "none"
  until F2 ships.
- `info.checksum_is_crc32c` — verifies the value is "crc32c" (F3 ready).

## Verification gates

- All 30 tests pass (was 29; +1 test_info).
- Existing integration tests still pass (info is sent automatically).

## Performance

The JSON is built via `snprintf` once per connection (not per frame).
Buffer size: 512 bytes (was 256). Allocation: none.

## Security

- The INFO frame does not leak secrets (TLS key, password).
- `auth:true` is advertised only when auth is configured (no leak of
  configuration intent).
- `server_id` is a static identifier; not a fingerprint.

## Limitations

- No JSON parser in the codebase — tests verify with `strstr` for
  key/value pairs. The wire format is the source of truth; any
  structural change is a wire-format change and must bump the
  protocol version.
- `go` (max idle timeout) is hardcoded to "20m" — should be
  configurable in a future PR.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.4 (F4 was the only feature
  the bundle got wrong — INFO IS emitted; F4 is now an extension).
- `docs/reviews/round2_deep_attack.md` A2 (INFOs not emitted claim).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F4.
- `docs/features/wire-checksum.md` (F3, which uses this info).
