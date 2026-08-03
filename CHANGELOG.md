# Changelog

All notable changes to CMSGQueue are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
- Build hardening (F7): FORTIFY_SOURCE=2, PIE, RELRO, stack-protector-strong
  with hot-path exclusion (cmq_parser.c, cmq_slab.c, cmq_mpool.c).
- INFO frame (F4): extended with capability bitmap (server_id, version,
  max_payload, auth, tls, compression, checksum, headers, batch).

### Added
- Wire checksum (F3): CMQ_FLAG_CHECKSUM with CRC32C verification. Trailing
  4 bytes of payload are the CRC32C of the rest. Standard form (RFC 3309).
- Hardware CRC32C (F9): cmq_crc32c module with SSE4.2 / aarch64 CRC32 hardware
  intrinsics. Software fallback for portability.
- 6 new tests: test_hardening, test_crc32c, test_checksum_wire, test_info
  (parser tests + new wire flags).

### Fixed
- F11 interop bug: parser now rejects CMQ_FLAG_COMPRESSED (silently
  round-tripped garbage before). CMQ_FLAG_CHECKSUM is now implemented
  (F3) and accepted by the parser.

### Security
- TLS plaintext stub (`cmq_tls_backend_secure()` returns 0) remains a
  fail-closed stub. **Not yet replaced with OpenSSL — F1 still pending.**
- Auth password (`auth_password`) still stored as plaintext via strdup
  at `src/server/cmq_config.c:124`. **argon2id migration (F8) blocked
  on libsodium headers — DEFERRED to v0.2.1.**

## [0.1.0] - 2026-07-25

Initial release.
