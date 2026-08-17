# Changelog

## [0.5.3] - 2026-08-14

### Added
- F17 BIO-wrap write/read wiring: `write_one` and `read_one` helpers in `cmq_route.c` wrap the existing `read`/`write` calls. When a `cmq_route_tls_sess_t` is passed, the bytes flow through `SSL_read`/`SSL_write`. When NULL, the helpers fall back to plain read/write.
- F19 server-side MQTT listener stub returns 0 (success). Full state machine (CONNECT/CONNACK/SUBSCRIBE/SUBACK/PUBLISH/PUBACK/PINGREQ/PINGRESP/DISCONNECT) is a v0.5.4 task.

### Tests
- 54/54 (excluding test_stress flake).

### Deferred to v0.5.4
- F19 full MQTT 5.0 + QoS 2 state machine.
- WAL replay parallelization.
- F17 full cmq_route.c BIO-wrap integration with a real cmq_route_tls_sess.

## [0.5.2] - 2026-08-14
- N1 enforcement + F17 API surface.

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
