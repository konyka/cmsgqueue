# Changelog

## [0.5.0] - 2026-08-14

### Added
- F18 wire-up: `cmq_server_create` opens the subscription persistence file when `persist_dir` is set. `handle_subscribe` records subs in the WAL. `handle_unsubscribe` records unsubs. Subscriptions survive a server restart.
- F14/F15/F16 enforcement: ACL check (`cmq_acl_check`) before sublist match; quota check (`cmq_quota_check_publish`) after; blocklist check (`cmq_blocklist_check`) at CONNECT. Reject with `cmq_send_error` on fail.
- N3 audit log test: verifies file creation when path is set.

### Tests
- 52/52 (was 49 in v0.4.0; +3 new test files).

### Deferred to v0.5.1
- F17 full BIO-wrap in cmq_route.c.
- F19 MQTT 5.0 + QoS 2.
- WAL replay parallelization.
- mTLS client cert chain.
- N1 per-subject rate limit.
- N2 hot config reload (SIGHUP).

## [0.4.0] - 2026-08-05
7 features: F1 (test fix), F12 (TLS hardening), F14/F15/F16 wire-up, F17 (TLS lib), F18 (persistence), F19 (MQTT lib), F7 (hardening).

## [0.3.0] - 2026-08-05
11 features + 2 stubs.

## [0.2.0] - 2026-08-03
11 features.

## [0.1.0] - 2026-07-25
Initial release.
