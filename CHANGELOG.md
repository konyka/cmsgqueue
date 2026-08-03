# Changelog

All notable changes to CMSGQueue are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.2.0] - 2026-08-03

### Added

- **F1 — TLS per-listener OpenSSL backend**. Per-listener `SSL_CTX`, TLS 1.2 floor, TLS 1.3 preferred, AEAD-only ciphers, `SSL_OP_NO_COMPRESSION`, non-blocking handshake.
- **F2 — Wire compression (zstd) at BATCH level**. `cmq_compress` module wrapping libzstd 1.5+. `CMQ_FLAG_COMPRESSED` honored on BATCH frames.
- **F3 — Wire checksum (CMQ_FLAG_CHECKSUM)**. CRC32C (RFC 3309) trailing 4 bytes. Built on F9 hw-acceleration.
- **F4 — Extended INFO frame with capabilities**. server_id, version, max_payload, auth, tls, compression, checksum, headers, batch.
- **F5 — Persistence WAL wired into server**. `persist_dir` config opens `cmq_filestore`. Best-effort; `stat_persist_fail` counter.
- **F6 — MQTT bridge wired into server lifecycle**. `mqtt_bridge_addr`/`mqtt_bridge_port` config opens the upstream client.
- **F7 — Build hardening**. FORTIFY_SOURCE=2, PIE, RELRO, stack-protector-strong (with hot-path exclusions for `cmq_parser.c`, `cmq_slab.c`, `cmq_mpool.c`).
- **F9 — Hardware CRC32C with software fallback**. `cmq_crc32c` module using `_mm_crc32_u64` (SSE4.2) / `__crc32cd` (aarch64).
- **F10 — Per-IP rate limit + per-conn subject cap**. `max_connects_per_sec` config, 1024-slot fixed-window table. Per-conn subject cap was already enforced.
- **F12 — `/healthz`, `/readyz` HTTP endpoints**.
- **F13 — Prometheus `/metrics` endpoint**. `cmq_connections`, `cmq_subscriptions`, `cmq_messages_in_total`, `cmq_messages_out_total`.
- **F15 — REQUEST/RESPONSE inbox HoL fix**. `inbox_max_pending` config; per-conn `inbox_pending` counter; reject on overflow.

### Fixed

- **F11 — Wire flag rejection**. Parser now rejects `CMQ_FLAG_COMPRESSED` pre-CONNACK. `CMQ_FLAG_CHECKSUM` was promoted from rejected to implemented (F3).

### Security

- TLS plaintext stub (`cmq_tls_backend_secure()` returns 0) — **fixed by F1** with real OpenSSL backend.
- `auth_password` plaintext via `strdup` at `cmq_config.c:124-125` — **NOT YET** fixed (F8 blocked on libsodium headers, deferred).
- Build hardening flags absent — **fixed by F7**.
- WIRE_FLAG silent-bug (compressed bytes fanned out as plaintext) — **fixed by F11 + F2** (rejection + zstd).

## [0.1.0] - 2026-07-25

Initial release.
