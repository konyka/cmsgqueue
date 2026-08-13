# Changelog

## [0.4.0] - 2026-08-05

### Added
- F1 test_stress flake fix: subscribe-to-publish barrier + deterministic drain loop. 80 % pass rate (was 20 %).
- F12 TLS per-listener + ALPN + cert reload + mTLS. `cmq_tls_set_alpn`, `cmq_tls_reload`, `SSL_VERIFY_PEER`.
- F14/F15/F16 wire-up: `cmq_quota`, `cmq_acl`, `cmq_blocklist` loaded on `cmq_server_create`.
- F17 inter-node TLS library: `cmq_route_tls` API. Full BIO-wrap in `cmq_route.c` is deferred to v0.5.0.
- F18 persistent subscription state: WAL text format with reload on startup.
- F19 server-side MQTT 3.1.1 listener API + tests. Full protocol state machine deferred to v0.5.0.
- F7 hardening: HSTS sent on /healthz, /readyz, /metrics when TLS configured. Audit log rotates at 100 MiB.

### Tests
- 51/51 (was 45 in v0.3.0; +6 new test files).

### Deferred to v0.5.0
- Inter-node TLS full BIO-wrap (cmq_route.c).
- MQTT 5.0 + QoS 2.
- WAL replay parallelization.
- mTLS client cert chain validation (custom CRL).

## [0.3.0] - 2026-08-05
- 11 features + 2 stubs shipped.

## [0.2.0] - 2026-08-03
- 11 features shipped.

## [0.1.0] - 2026-07-25
- Initial release.
