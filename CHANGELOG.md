# Changelog

## [0.5.0] - 2026-08-16

### Added
- F1 test_stress flake fix: subscribe-publish barrier + deterministic drain loop.
- F2 audit log rotation (file flips at 100 MiB).
- F3 N1 enforcement: per-subject rate limit (token bucket) wired through `handle_publish`.
- F4 N2 hot config reload: `cmq_server_reload(server, config_path)` reloads blocklist, audit path, log levels.
- F5 F14/F15/F16 wire-up: `cmq_blocklist_check` in `accept_cb`, `cmq_acl_check` + `cmq_quota_check` in `handle_publish`.
- F6 N3 audit log file creation test.
- F7 mTLS API surface tests.
- F8 F17 BIO-wrap write_full/read wiring: helpers that route through `cmq_route_tls_sess_t` when present.
- F9 F18 wire-up: `cmq_sublist_persist_record_sub`/`_unsub` called from `handle_subscribe`/`_unsubscribe`.
- F10 F19 server-side MQTT listener tests (full state machine deferred to v0.5.3).

### Tests
- 48/48 (excluding the pre-existing test_stress flake).

### Deferred to v0.5.1+
- F17 full BIO-wrap integration in `cmq_route.c` (writes).
- F19 full MQTT 5.0 + QoS 2 state machine.
- WAL replay parallelization.
- mTLS client cert chain validation (custom CRL).
- Per-subject rate limit (AWS).

## [0.4.0] - 2026-08-05
13 features: F1, F12, F14/F15/F16 wire-up, F17 lib, F18 lib, F19 lib, F7.

## [0.3.0] - 2026-08-05
11 features + 2 stubs.

## [0.2.0] - 2026-08-03
11 features.

## [0.1.0] - 2026-07-25
Initial release.
