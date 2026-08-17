# Changelog

## [0.5.2] - 2026-08-14

### Added
- N1 enforcement: per-subject rate limit consulted in `handle_publish` after F14 quota. Config: `max_msgs_per_sec_per_subject`.
- F17 API surface: `cmq_route_tls_sess` module wraps an SSL session for route connections. The full BIO-wrap in `cmq_route.c` is deferred to v0.5.3.

### Tests
- 55/55 (was 53 in v0.5.1; +2 new test files).

### Deferred to v0.5.3
- F19 full MQTT 5.0 + QoS 2 (stub returns success; full state machine pending).
- F17 full socket BIO-wrap in `cmq_route.c`.
- WAL replay parallelization.

## [0.5.1] - 2026-08-14
- N2 hot config reload + N1 library + mTLS tests.

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
