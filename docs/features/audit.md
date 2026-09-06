# F13: Structured Audit Log

## Motivation

Operational visibility requires a separate event stream for security-sensitive actions (auth, persist, TLS). The existing `cmq_log` is general-purpose; audit events need a structured format that operators can grep, parse, and route to a SIEM.

## Design

A new `cmq_audit` module writes JSON-lines events. Each event is one line. Format:

```json
{"ts":"2026-08-05T08:23:01.669Z","event":"auth_ok","trace":"<trace-id>","subject":"<user>","details":"<details>"}
```

Fields:
- `ts` — UTC ISO 8601 with millisecond precision.
- `event` — one of: `auth_ok`, `auth_fail`, `persist_fail`, `persist_recover`, `tls_handshake_fail`, `rate_limit_reject`.
- `trace` — hex of the connection's 16-byte trace ID (F11), or empty for non-connection events.
- `subject` — the user, account, or IP the event is about.
- `details` — free-form context.

Output destinations: stderr (always) and an optional audit file
(`cmq-audit.log` in `persist_dir`). v0.5.129: create calls
`cmq_audit_from_persist` after the WAL opens. Destroy clears
the path. Reload applies a non-empty `persist_dir`; omitted
or empty keeps the current file. `..` / `\` / controls fail
closed.

v0.5.51 wires the remaining events at their natural sites
(CONNECT auth, WAL append/replay, TLS handshake). Successful
PUBLISH does not call the auditor. `cmq_audit_auth` never
takes a password.

JSON escaping: `"` and `\\` are escaped, control chars (including `\n`/`\r`) are escaped as `\n`/`\r`, and other low-ASCII bytes as `\uXXXX`.

## Files touched

- `src/enterprise/cmq_audit.{h,c}` (new).
- `CMakeLists.txt` — `cmq_audit.c` added to `CMQ_ENTERPRISE_SOURCES`.
- `tests/test_audit.c` (new).

## Tests

`tests/test_audit.c`:
- `audit.set_path_disables_when_null` — file output off.
- `audit.log_writes_event_to_stderr` — exercise.
- `audit.log_writes_event_to_file` — file contains expected substrings.
- `audit.json_escape_special_chars` — quote/backslash/newline escape.
- `audit.event_names` — enum → string map, including unknown.
- `audit.auth_helper_no_secret` — `cmq_audit_auth` writes ok/fail.

`tests/test_adt.c` (v0.5.129):
- `adt.apply` — `{dir}/cmq-audit.log` receives the event.
- `adt.omitted` / `adt.empty` — reload keeps the current file.
- `adt.reject` — `..` / `\` leave the current file.

## Verification gates

- 4/4 audit tests pass.
- 45/45 total tests pass.

## Performance

Audit log writes are O(1) per event. The cost is dominated by `fputs` to stderr (~1 µs per line). Operators monitor throughput; at 1000 events/sec, the overhead is 1 ms/sec.

## Security

Threats closed:
- **Audit trail for forensics** — auth failures, persist failures, rate-limit rejections are now logged with structured metadata.
- **SIEM integration** — JSON-lines format is standard for log shippers (filebeat, fluentd, vector).

Threats NOT closed:
- **Audit log tampering** — the file is plaintext. Production should chmod 0600 and stream to a remote syslog.
- **Audit log overflow** — no size cap. Production should use logrotate.

## Limitations

- No structured fields for trace ID propagation beyond the connection-level (F11).
- Audit events are best-effort: a stderr write failure is silently dropped.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F13.
- `docs/features/password-hash.md` — F8 CONNECT calls `cmq_audit_auth` (v0.5.51).
- `docs/features/blocklist.md` — F15 calls `cmq_audit_log(CMQ_AUDIT_RATE_LIMIT_REJECT, ...)`.
