# Changelog

## [0.5.1] - 2026-08-14

### Added
- N2: `cmq_server_reload(server, config_path)` API. Re-reads config; hot-swaps blocklist, updates audit path, log levels. No listener threads stopped.
- N1: `cmq_subject_rl` library (per-subject token bucket). `cmq_subject_rl_create(max_per_sec)`, `cmq_subject_rl_check(rl, subject)`, `cmq_subject_rl_free(rl)`.
- mTLS API tests: `cmq_tls_set_verify` + `cmq_tls_set_ca` + `cmq_tls_verify_peer` validated.

### Tests
- 53/53 (was 52 in v0.5.0; +3 new test files).

### Deferred to v0.5.2
- F17 full BIO-wrap in `cmq_route.c`.
- F19 MQTT 5.0 + QoS 2.
- WAL replay parallelization.
- N1 server-side enforcement (library exists, wiring is follow-up).

## [0.5.0] - 2026-08-14
- F18 wire-up + F14/F15/F16 enforcement + N3 audit test.

## [0.4.0] - 2026-08-05
- 7 features: F1, F12, F14/F15/F16 wire-up, F17 lib, F18 lib, F19 lib, F7.

## [0.3.0] - 2026-08-05
- 11 features + 2 stubs.

## [0.2.0] - 2026-08-03
- 11 features.

## [0.1.0] - 2026-07-25
- Initial release.
