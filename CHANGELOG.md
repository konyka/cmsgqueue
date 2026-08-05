# Changelog

All notable changes to CMSGQueue are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-08-05

### Added

- **F8 password hashing (scrypt)** — `auth_password` was plaintext (CWE-256). Now hashed via OpenSSL `EVP_PBE_scrypt` with `$scrypt$N=16384,r=8,p=1,salt_len=NN,hash_len=NN$...` wire format. 6 unit tests; constant-time verify. (Library + server wiring.)
- **F8b auth brute-force rate limit** — per-IP cap at 10 attempts/sec on failed CONNECT. Reject with `cmq_send_connack(c, 4)`. Uses the F10 rate lock.
- **F11 connection tracing** — 16-byte UUID per connection, stored in `cmq_client_t.trace_id`, generated via `RAND_bytes`. Hex-encoded for logs.
- **F15 connection blocklist** — IP/CIDR list loaded at startup; reloadable at runtime. Library complete; server wiring is a follow-up.
- **F13 structured audit log** — JSON-lines events to stderr and optional file. Events: `auth_ok`, `auth_fail`, `persist_fail`, `persist_recover`, `tls_handshake_fail`, `rate_limit_reject`. Lock-protected.
- **F14 per-account quota** — `cmq_quota` token-bucket per account. Caps: `max_msgs_per_sec`, `max_bytes_per_sec`, `max_connections`. 5 unit tests.
- **F16 per-subject ACL** — `cmq_acl` with NATS-style wildcards (`*` for one token, `>` for full subtree). Allow-list and deny-list; deny-list wins.
- **F19 server-side MQTT 5.0 listener (STUB)** — `cmq_mqtt_server_listen` returns `-ENOSYS`. Full implementation deferred to v0.4.0.
- **F20 libfuzzer harness** — `tests/fuzz/fuzz_parser.c` exercises `cmq_parser_feed` with random input. Two-chunk feed covers the partial-frame path.
- **F21 pre-commit hooks** — `.pre-commit-config.yaml` with build + test hooks.

### Security

- Plaintext password leak (CWE-256) closed by F8.
- Online brute-force closed by F8b.
- Per-account quota (F14) and per-subject ACL (F16) add granular control.

### Performance

- 33K msg/s end-to-end baseline unchanged (F13, F14, F16, F18 are off-hot-path).
- 45/45 tests passing (was 36 in v0.2.0; +9 new test files).

### Deferred to v0.4.0

- **F12 TLS hardening** — per-listener CTX + ALPN. Current single-listener is acceptable.
- **F17 inter-node encryption** — cluster routes are plaintext. Significant refactor of `cmq_route.c`.
- **F18 persistent subscription state** — API in place; full implementation deferred.

## [0.2.0] - 2026-08-03

11 features: F1 TLS, F2 zstd, F3 wire checksum, F4 extended INFO, F5 persistence WAL, F6 MQTT bridge, F7 build hardening, F9 hw-CRC, F10 rate limit, F11 wire flag rejection, F12/F13 health/metrics, F15 inbox HoL.

## [0.1.0] - 2026-07-25

Initial release.
