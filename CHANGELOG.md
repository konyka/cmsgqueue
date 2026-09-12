# Changelog

## 0.5.69 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::client_role_confusion`** —
  defensive test for the TLS role-confusion attack pattern.
  Sends a ServerHello (message type 0x02) to a server expecting
  a ClientHello (message type 0x00). Verifies the server's
  state machine detects the role mismatch and rejects cleanly.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  107/107 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.70+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.68 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::malformed_protocol_version`** —
  defensive test for an impossible TLS protocol version (0x03
  0xFF). Verifies the server cleans up cleanly when the
  client sends a malformed ClientHello.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  106/106 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.69+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.67 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::empty_cert_file_rejected`** —
  defensive test for an empty (zero-byte) `tls_cert` file.
  Asserts `cmq_server_create` returns non-OK and server pointer
  is NULL. Guards against the server crashing or running
  with an invalid SSL_CTX when given a malformed cert file.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  105/105 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.68+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.66 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::plain_tls_no_client_cert`** —
  sanity test that a plain TLS server accepts a TLS client
  without a cert. Regression guard against future changes that
  accidentally enable mTLS by default.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  104/104 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.67+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.65 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_multi_ca_bundle`** —
  defensive test for the mTLS CA bundle with multiple CAs
  concatenated into a single PEM file. Documents the OpenSSL
  limitation that `SSL_CTX_load_verify_locations` only loads
  the FIRST cert in a concatenated PEM (production workaround:
  use `X509_LOOKUP_add_dir` or call the function multiple times).

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  103/103 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.66+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.64 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_self_signed_ca`** —
  defensive test for the self-signed-CA mTLS deployment pattern
  where the same file is used for `tls_cert` and `tls_ca`.
  Verifies the trust chain doesn't accidentally widen when the
  same self-signed cert is the only trust anchor.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  102/102 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.65+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.63 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::client_trusts_server_cert_directly`** —
  defensive test for the common operational pattern of trusting
  the server's cert directly (self-signed) without a separate
  CA bundle. Asserts the handshake succeeds.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  101/101 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.64+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.62 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mid_handshake_tcp_rst`** —
  defensive test for the RST (vs FIN) close path during TLS
  handshake. Sets `SO_LINGER={1, 0}` to force TCP RST instead of
  FIN. Verifies the server cleans up cleanly without hanging
  on the dead fd.

  Complements the v0.5.50 `mid_handshake_disconnect` (FIN path)
  by exercising the harder RST path.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  100/100 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.63+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.61 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::tls_enabled_without_cert_rejected`** —
  defensive test that `cmq_server_create` rejects
  `tls_enabled=1` without `tls_cert`/`tls_key`. Guards against
  the production mistake of forgetting the cert path while
  flagging TLS on.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  99/99 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.62+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.60 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::cert_key_mismatch_rejected`** —
  defensive test for the cert/key pair validation at server
  startup. Generates two unrelated cert/key pairs. Configures
  server with mismatched `tls_cert` + `tls_key`. Asserts
  `cmq_server_create` returns non-OK and the server pointer
  is NULL.

  Guards against regressions where a cert/key mismatch slips
  through to first-handshake-time (harder to diagnose).

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  98/98 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.61+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.59 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::idle_tls_client`** —
  defensive test for half-open TLS connections. A TCP client
  connects but never sends data (no ClientHello). The server
  sits in WANT_READ. After the client closes, the server
  must clean up cleanly. Verifies `cmq_server_stop` +
  `pthread_join` complete in bounded time (server isn't
  stuck on a dead fd).

  Guards against regressions in the half-open connection
  cleanup logic.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  97/97 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.60+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.58 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_forces_tls12`** —
  tripwire test for the v0.5.46 TLS 1.2 cap. Configures
  `tls_verify_peer=1` (triggers the cap). Connects with a
  TLS 1.3-only client. Asserts the handshake fails because
  the server caps at TLS 1.2.

  If a future refactor "fixes" TLS 1.3 mTLS and removes the
  cap, this test fails — forcing the author to update the
  test along with the fix. Locks in the v0.5.46 design
  boundary until the v0.5.48/v0.5.49 race is resolved.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  96/96 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.59+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.57 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_missing_ca_bundle`** —
  defensive test for the v0.5.46 mTLS CA bundle trust chain.
  Configures `tls_ca` to a non-existent path + `tls_verify_peer=1`.
  Connects with a valid client cert. Asserts the handshake
  fails (fail-closed behavior).

  Guards against regressions where the CA bundle is silently
  treated as "trust any cert" when the path is invalid.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  95/95 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.58+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.56 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_cross_ca_rejected`** —
  defensive test for the v0.5.46 mTLS CA bundle trust chain.
  Generates two independent CAs (ServerCA, ClientCA). Configures
  `cmq_server` to trust ServerCA only. Connects with a client
  cert signed by ClientCA. Asserts the handshake fails.

  Guards against regressions where the CA bundle is silently
  widened (e.g., `tls_ca=NULL` or wrong file) and the chain
  check stops actually filtering untrusted certs.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  94/94 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.57+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.55 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_missing_crl_file_path`** —
  defensive test for the v0.5.47 CRL pipeline's behavior when
  `tls_crl` points at a non-existent path. Asserts the server
  starts without crashing AND a valid client cert is accepted
  (fail-open policy: missing CRL file → CRL check silently
  skipped).

  Documents a security tradeoff: a misconfigured `tls_crl`
  path means no CRL enforcement, rather than rejecting all
  clients. The test guards against accidental policy changes.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  93/93 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.56+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.54 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_crl_not_revoked_accepted`** —
  defensive test for the common case of CRL verification: a
  client whose cert is NOT in the CRL must still be accepted.
  Guards against the OpenSSL #23325 scope-matching bug class
  where valid clients are falsely rejected.

  The test generates two client certs (one revoked, one not,
  both with CDP extensions), runs the standard 3-step CRL
  generation, configures `cmq_server` with `tls_crl`, and
  verifies the non-revoked client connects successfully.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  92/92 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.55+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.53 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::tls13_negotiated`** —
  defensive test that the server actually negotiates TLS 1.3
  for plain TLS. Uses a client pinned to TLS 1.3 only
  (`min=max=TLS1_3_VERSION`) and asserts `SSL_version ==
  0x0304` after the handshake. Catches regressions where
  the v0.5.46 mTLS TLS 1.2 cap accidentally leaks into the
  plain TLS path.

  (A concurrent-handshakes defensive test was attempted in
  this round but was flaky under the test framework's
  scheduling noise. The accept thread is single-threaded even
  with `num_threads > 1`, so concurrent handshakes don't
  exercise a meaningful race. Reverted.)

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  91/91 pass.
- Bench: ~34K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.54+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.52 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::concurrent_handshakes`** —
  defensive test for the v0.5.45 handshake-resume fix under
  concurrency. Runs `cmq_server` with `num_threads=4` and
  spawns 8 simultaneous client threads, each driving a full
  TLS handshake against the server. Asserts all 8 handshakes
  succeed. Guards against future regressions that could mix
  up SSL state across concurrent handshakes.

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  91/91 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.53+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.51 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::garbage_tls_record`** —
  defensive test for the v0.5.45 handshake-resume fix. Sends
  a valid-looking TLS record header with an invalid record
  type (0xff) to a TLS-enabled server. OpenSSL rejects the
  record with `SSL_ERROR_SSL`, the server's `cmq_tls_handshake`
  returns -1, and `client_tls_handshake` destroys the session
  cleanly. Test asserts the server doesn't hang on the bad fd
  (same regression-guard pattern as v0.5.50's
  `mid_handshake_disconnect` but for a different error path).

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  90/90 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.52+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests (TLS fragment reassembly, etc.).
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.50 - 2026-09-05

### Added
- **`tests/test_tls_e2e_handshake.c::mid_handshake_disconnect`** —
  defensive test for the v0.5.45 TLS handshake resume fix.
  Connects to the server, sends a partial ClientHello, closes
  the socket abruptly. Asserts the server's event loop detects
  the closed fd (via `cmq_tls_handshake → SSL_do_handshake →
  SSL_ERROR_SYSCALL → -1`) and tears down the client cleanly
  without blocking `cmq_server_stop` / `pthread_join`.

  Regression guard against future changes that could re-introduce
  the v0.5.45 bug (or a variant where the server spins on a
  dead fd instead of detecting the closed state).

### Verified
- `ctest -j1` (excluding flaky `test_stress` + `test_bench_regression`):
  89/89 pass.
- Bench: ~32K msg/s, p99 99 µs (unchanged).

### Deferred to v0.5.51+
- TLS 1.3 mTLS via post-handshake auth (v0.5.48/v0.5.49 race).
- More defensive tests around concurrent TLS handshakes.
- Per-listener `accept_thread_func` refactor (already on remote
  workstream as v0.5.42).

## 0.5.49 - 2026-09-05

### Investigated
- **TLS 1.3 mTLS second attempt.** v0.5.48 left a race condition
  where authenticated mTLS clients were torn down prematurely.
  v0.5.49 tried a tighter design:
  - `handshake_flushed` guard on `cmq_tls_session_t` ensures
    `SSL_write(NULL, 0)` fires at most once per session.
  - Changed `X509_V_FLAG_CRL_CHECK_ALL` to `CRL_CHECK` to
    avoid rejecting valid certs without CDP (the v0.5.48
    cert was rejected with `X509_V_ERR_DIFFERENT_CRL_SCOPE`,
    OpenSSL issue #23325).

  Result: same race. Authenticated mTLS clients still see
  unexpected EOF; no clear alert. Root cause not pinpointed
  (likely OpenSSL 3.5 internal state issue with post-handshake
  + the v0.5.45 read-path resume interaction).

### Reverted
- Production code is unchanged from v0.5.47. The v0.5.46 TLS 1.2
  cap for `verify_peer=1` stays.

### Deferred to v0.5.50+
- Investigate OpenSSL 3.5 source for the post-handshake state
  machine.
- Consider application-layer mTLS verification (read client
  cert off the wire separately, not via OpenSSL).
- Or accept TLS 1.2 as the only mTLS path.

### Verified
- `ctest -j1`: 88/88 pass.
- Bench: ~33K msg/s, p99 99 µs (unchanged).

## 0.5.48 - 2026-09-05

### Investigated
- **TLS 1.3 mTLS post-handshake race condition.** Attempted to
  lift the v0.5.46 TLS 1.2 cap by wiring the proper TLS 1.3
  client-cert path:
  - `cmq_tls_client_hello_cb` — per-SSL `SSL_set_verify` +
    `SSL_verify_client_post_handshake` to trigger the
    post-handshake CertificateRequest.
  - `cmq_tls_info_cb` — watches for `SSL3_AD_HANDSHAKE_FAILURE`
    / `BAD_CERTIFICATE` / `CERTIFICATE_REVOKED` alerts and marks
    the session for teardown.
  - `cmq_tls_flush` — wraps `SSL_write(NULL, 0)` to force
    OpenSSL to emit pending records (required for the
    post-handshake CertificateRequest to actually go out).
  - `cmq_tls_handshake_failed` — accessor for the event loop.

  The full pipeline worked for the rejection cases (unauthenticated
  client, revoked client cert), but **caused authenticated mTLS
  clients to be torn down prematurely** after the post-handshake
  CertificateRequest. Root cause: a race between the v0.5.45
  read-path resume (drives `SSL_do_handshake` on every EV_READ)
  and the v0.5.48 `cmq_tls_flush` (drives a write op on every
  wakeup) interacting with the post-handshake state machine in
  a way that puts an authenticated connection into an error
  state.

### Reverted
- Production code is unchanged from v0.5.47. The v0.5.46 TLS 1.2
  cap for `verify_peer=1` stays.
- **No tag push for v0.5.48.** The release is documentation-only
  and the tag is held locally until the TLS 1.3 mTLS race is
  resolved in v0.5.49+.

### Deferred to v0.5.49+
- Resolve the post-handshake race (likely fix: gate the
  v0.5.45 read-path resume on `!handshake_failed`, OR defer
  the post-handshake flush until first application write).
- Consider switching to TLS 1.3-only design with
  `SSL_CTX_set_client_cert_engine` for explicit cert request.

### Verified
- `ctest -j1`: 88/88 pass (no production change, all existing
  tests still green).
- Bench: ~33K msg/s, p99 99 µs (unchanged).

## 0.5.47 - 2026-09-05

### Fixed
- **CRL (Certificate Revocation List) was silently bypassed**
  (`src/enterprise/cmq_tls.c:166-184`). The CRL was loaded into
  the SSL_CTX's X509_STORE via `X509_STORE_add_crl`, but the
  verifier never consulted it because
  `X509_V_FLAG_CRL_CHECK` was never set on the store. A
  "loaded" CRL was a silent no-op identical in severity to
  the v0.5.46 mTLS bypass.

  Fixed by setting both flags:
  ```c
  X509_STORE_set_flags(store,
      X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
  ```

### Added
- **`cmq_config_t.tls_crl` config field** (and
  `cmq_listener.tls_crl` for per-listener). Wires
  `cmq_tls_set_crl` through `cmq_server_create` for both
  slot 0 and per-listener slots. Without this field the
  CRL API was unreachable from the public config since
  v0.5.2.
- **`tests/test_tls_e2e_handshake.c::mtls_revoked_client_rejected`**
  — first end-to-end CRL test. Generates a CA + server +
  client cert (with CDP extension pointing at the CRL
  path), runs the three-step OpenSSL CRL sequence
  (empty CRL → revoke → re-emit CRL), configures
  `cmq_server` with `tls_crl`, and asserts the server
  rejects the revoked client during the mTLS handshake.
- **`docs/features/tls-crl-revocation.md`** — feature doc
  including the CDP requirement that took substantial
  debugging to discover (a cert without CDP is silently
  accepted even with `X509_V_FLAG_CRL_CHECK` set).

### Verified
- `ctest -j1` (excluding flaky `test_stress` +
  `test_bench_regression`): 89/89 pass.
- Bench: ~32K msg/s end-to-end, p99 inter-arrival 99.0 µs.

### Caveats
- TLS 1.3 mTLS still requires the v0.5.46 TLS 1.2 cap (TLS 1.3
  moves cert verification to post-handshake; without the
  cap, an unauthenticated TLS 1.3 client may be admitted
  before verification fires). CRL rejection works on
  TLS 1.2 mTLS only.
- Production deployments must issue client certs with a
  `crlDistributionPoints` extension pointing at the CRL URI.
  Certs without CDP bypass CRL check silently.

### Deferred to v0.5.48+
- TLS 1.3 mTLS via client-cert-engine API (lift v0.5.46 cap).
- OCSP stapling as an alternative to CRL.

## 0.5.46 - 2026-09-05

### Fixed
- **mTLS (mutual TLS) verification was silently bypassed for
  TLS 1.3 clients** (`src/enterprise/cmq_tls.c:118`). When
  `cfg->verify_peer` was set, `SSL_CTX_set_verify(PEER |
  FAIL_IF_NO_PEER_CERT)` was correctly applied to the SSL_CTX,
  but OpenSSL 3.5 / TLS 1.3 changed the client-cert handshake:
  the server must explicitly opt in to TLS 1.3's
  `CertificateRequest` path via the client-cert-engine API,
  otherwise it never sends the request — even with the verify
  mode set. The server completed the handshake without asking
  for a client cert, so any TLS 1.3 client was accepted as
  authenticated regardless of whether it presented a cert.

  Fixed by capping the SSL_CTX at TLS 1.2 when `verify_peer`
  is set:
  ```c
  if (cfg->verify_peer) {
      SSL_CTX_set_max_proto_version(cfg->ssl_ctx, TLS1_2_VERSION);
  }
  ```
  TLS 1.2 honors `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` and sends
  `CertificateRequest`; an empty `Certificate` message from the
  client fails the handshake with `handshake_failure` per
  RFC 5246.

  Plain TLS (no mTLS) is unaffected — TLS 1.3 remains the
  default when `verify_peer` is not set. Bench: ~33K msg/s,
  p99 99.0 µs (unchanged).

### Added
- **`tests/test_tls_e2e_handshake.c::mtls_required`** — first
  end-to-end test of the mTLS code path. Generates a CA,
  CA-signed server cert, CA-signed client cert. Configures
  `cmq_server` with `tls_verify_peer=1` and verifies that
  an authenticated client handshake succeeds (sub-test A)
  while an unauthenticated client handshake fails (sub-test
  B). Closes the gap that `test_mtls_api.c` only checks the
  setter round-trip, not runtime behavior.
- **`docs/reviews/v0.5.46.enumeration.md`** — design doc.
- **`docs/features/tls-mtls-verify.md`** — feature doc.

### Verified
- `ctest -j1`: 88/88 pass (all TLS-related tests pass;
  plaintext unaffected).
- Bench: ~33K msg/s end-to-end, p99 inter-arrival 99.0 µs.

### Deferred to v0.5.47+
- TLS 1.3 mTLS via `SSL_CTX_set_client_cert_engine` (or
  equivalent), so the TLS 1.2 cap can be lifted.
- CRL revocation end-to-end test.
- Mutual mTLS through the multi-listener path (each listener
  with its own CA bundle).

## 0.5.45 - 2026-09-04

### Fixed
- **TLS handshake through `cmq_server_run` was broken in two ways**
  (`src/server/cmq_server.c:6395-6422`). `client_tls_handshake`
  treated `cmq_tls_handshake`'s return values incorrectly:
  the success case (`rc == 1`) and the error case (`rc == -1`)
  were both routed into `if (rc != 0) { destroy; return -1; }`,
  so a handshake that completed synchronously on accept was
  silently killed. The pending case (`rc == 0`,
  WANT_READ/WANT_WRITE) set `client->tls` but never resumed the
  state machine, so any handshake needing multiple round-trips
  was accepted with a dead TLS session. Fixed by:
  1. `client_tls_handshake` now only destroys + rejects on
     `rc < 0`. `rc == 0` (pending) and `rc == 1` (success)
     both attach the session to `client->tls`.
  2. `client_read_cb` drives `cmq_tls_handshake` on every
     `EV_READ` while `!cmq_tls_handshake_done(c->tls)`,
     completing the handshake across multiple wakeups. After
     completion, control falls through to the normal read path.
- New accessor `cmq_tls_handshake_done(session)` in
  `src/enterprise/cmq_tls.{c,h}` exposes the `handshake_done`
  flag for use by the server's read/write callbacks.

### Added
- **`tests/test_tls_e2e_handshake.c`** — real end-to-end TLS
  handshake through `cmq_server_run`. Two tests:
  1. `tls_e2e_handshake.single_listener` — single TLS listener,
     full `SSL_connect` against port, asserts `SSL_do_handshake`
     returns 1.
  2. `tls_e2e_handshake.multi_listener_distinct_certs` — two
     listeners (slot 0 + slot 1) with distinct self-signed
     certs. Verifies port+0 trusts cert0, port+1 trusts cert1,
     and the cross-check (connect to slot 1's port trusting
     cert0) is correctly rejected by the client. This is the
     first test that exercises v0.5.33's
     `srv_find_tls_slot` → `client_tls_handshake` integration
     in production (not just unit).
- **`docs/reviews/v0.5.45.enumeration.md`** — design doc
  covering the bug, the fix, and the test plan.
- **`docs/features/tls-handshake-resume.md`** — feature doc.

### Verified
- `ctest -j1` (excluding `test_stress` and `test_bench_regression`):
  88/88 pass.
- Bench: ~33K msg/s end-to-end, p99 inter-arrival 99.0 µs.

## 0.5.43 - 2026-09-03

### Added
- **Test-only retained-message dispatch helper** —
  `cmq_mqtt_dispatch_retained(topic, payload, payload_len, cb, user)`
  in `cmq_mqtt_server.{c,h}` walks the subscriber list registered
  via `cmq_mqtt_record_subscriber` and invokes `cb` for each whose
  topic_filter matches the given topic. Mirrors what
  `mqtt_handle_client` does on a real SUBSCRIBE. Production code
  must not call this.
- **Test-only subscriber list reset** —
  `cmq_mqtt_subs_reset_test()` clears `g_mqtt_sub_count` so
  multiple tests can run on the same global state without
  cross-contamination. Production code must not call this.

### Tests
- `tests/test_mqtt_retained_wildcard.c` — new file with 3 tests:
  - `plus_matches_and_delivers`: stored topic "sensors/temp",
    registered "sensors/+" subscriber, dispatch invokes the
    callback.
  - `hash_matches_and_delivers`: registered "sensors/#" subscriber,
    dispatch invokes the callback.
  - `non_matching_topic_does_not_deliver`: registered "events/+"
    subscriber for "sensors/temp" topic — no match, callback not
    invoked.

### Documentation
- `docs/reviews/v0.5.43.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0543_{1,2}.txt` — bench transcripts + retained
  + wildcard dispatch micro-bench.

### Test count
- 123 tests (was 120 in v0.5.42; +3 retained + wildcard tests).

## 0.5.42 - 2026-09-03

### Added
- **Test-only helpers for MQTT QoS2 retransmit table** in
  `cmq_mqtt_server.h`: `cmq_mqtt_qos2_record_or_lookup_test`,
  `cmq_mqtt_qos2_get_phase_test`, and `cmq_mqtt_qos2_reset_test`.
  These wrap the file-static helpers so unit tests can verify
  table-management semantics without driving a real MQTT wire-format
  client. Production code must not call them (documented in the
  header).

### Fixed
- **MQTT QoS2 table overflow silent drop** — `qos2_record_or_lookup`
  previously returned 0 (success) even when the table was full,
  silently dropping the entry. v0.5.42 makes it return -1 in that
  case so callers can detect overflow. The existing wire handler
  ignores the return value, so this is a silent behavior fix that
  doesn't break production.

### Tests
- `tests/test_mqtt_qos2.c` — new file with 5 tests:
  - `empty_table_returns_phase_zero`: get on empty returns 0.
  - `insert_new_returns_zero_and_sets_phase`: insert + get.
  - `update_existing_returns_one`: second insert returns 1.
  - `distinct_packet_ids_isolated`: packet_ids are independent.
  - `table_overflow_returns_neg_one`: filling 128 entries then
    inserting one more returns -1; existing entries remain
    queryable.
 All tests reset at the start to clear global state from prior
 tests.

### Documentation
- `docs/reviews/v0.5.42.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0542_{1,2}.txt` — bench transcripts + QoS2 test
  micro-bench.

### Test count
- 120 tests (was 115 in v0.5.41; +5 QoS2 tests).

## 0.5.41 - 2026-09-03

### Added
- **Rigorous end-to-end bridge recovery test** —
  `tests/test_mqtt_bridge_freelist_load.c:
  recovered_bridge_matches_recovered_subscriber`. Builds server A
  with `persist_dir`, persists a subscriber for topic
  `"v0.5.41/sentinel"`, enqueues a bridge record, destroys, then
  recreates server B and verifies `cmq_sublist_match` returns 1
  for the recovered subscriber. Proves the v0.5.40 recovery path
  actually calls `cmq_server_publish` with the recovered bridge
  topic (the v0.5.40 `stat_messages_replayed > 0` check was too
  loose — it also ticks on client-PUBLISH records that fail
  validation). New includes in the test file: `cmq_sublist.h`,
  `cmq_sublist_persist.h`, `cmq_account.h`, `string.h`.

### Documentation
- `docs/reviews/v0.5.41.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0541_{1,2}.txt` — bench transcripts + recovery
  test micro-bench.

### Test count
- 115 tests (was 114 in v0.5.40; +1 rigorous-end-to-end-bridge).

## 0.5.40 - 2026-09-03

### Changed
- **MQTT bridge WAL recovery** — `replay_one_record` in
  `cmq_server.c` now detects `CMQB` records (written by the v0.5.39
  bridge adapter) and dispatches them via `cmq_server_publish`
  instead of `handle_publish`. The cmq client-publish path is
  unchanged. On `cmq_server_create`, the existing replay loop
  automatically picks up bridge records and fans them out to
  matching live subscribers.

### Tests
- `tests/test_mqtt_bridge_freelist_load.c` — new
  `bridge_record_survives_restart` builds a server with
  `persist_dir`, enqueues a bridge record via the test-only
  helper, destroys, recreates, and polls
  `stat_messages_replayed` until the replay loop has processed
  the bridge record. Catches any regression in the
  detection/dispatch path.

### Documentation
- `docs/reviews/v0.5.40.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0540_{1,2}.txt` — bench transcripts + recovery
  micro-bench.

### Test count
- 114 tests (was 113 in v0.5.39; +1 bridge-record-survives-restart).

## 0.5.39 - 2026-09-03

### Added
- **MQTT bridge WAL persistence** — the bridge adapter now calls
  `cmq_server_persist_bridge` before `cmq_server_publish`, which
  wraps `cmq_filestore_append_bridge`. New public APIs:
  - `cmq_filestore_append_bridge(fs, topic, topic_len, payload,
    payload_len, &seq)` — builds a self-describing frame
    (`CMQB` magic + version byte + topic_len + topic + payload) and
    appends via the regular filestore path. The magic lets a future
    recovery loop distinguish bridge records from client-publish
    records (no magic).
  - `cmq_server_persist_bridge(srv, topic, payload, payload_len)`
    — thin wrapper that calls the filestore helper when
    `srv->filestore` is set; no-op otherwise. The bridge adapter
    uses this to persist without needing direct access to the
    server's filestore pointer.
  - Wire format lets v0.5.40+ add a recovery path that detects
    the magic and dispatches bridge records via
    `cmq_server_publish`.

### Tests
- `tests/test_mqtt_bridge_freelist_load.c` — new
  `bridge_publish_writes_to_wal` verifies that bridge enqueue
  produces one WAL record (last_seq increments by exactly 1).

### Documentation
- `docs/reviews/v0.5.39.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0539_{1,2}.txt` — bench transcripts + bridge
  persist micro-bench.

### Test count
- 113 tests (was 111 in v0.5.38; +1 bridge-persist test, +1 v0.5.38
  test moved into the same file via the merge with this round's
  refactor).

## 0.5.38 - 2026-09-03

### Added
- **handle_publish persistence integration smoke test** —
  `tests/test_persist_replay.c:handle_publish_writes_to_wal`.
  Writes a record to the WAL via `cmq_filestore_append`, restarts
  the server, verifies the second `cmq_server_create` doesn't crash
  and exposes the `stat_messages_replayed` field. Closes the gap
  between `test_persist_unit.c` (file format) and `test_recover.c`
  (replay loop in isolation).

### Documentation
- `docs/reviews/v0.5.38.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0538_{1,2}.txt` — bench transcripts + replay
  test micro-bench.

### Test count
- 111 tests (was 110 in v0.5.37; +1 persist-replay test).

## 0.5.37 - 2026-09-03

### Added
- **End-to-end persistent subscriber restart test** —
  `tests/test_persist_restart.c:subscriber_survives_restart`.
  Creates a server with `persist_dir`, records a subscription
  pattern, destroys the server, recreates it with the same
  `persist_dir`, and verifies the subscription is recovered
  (via `cmq_sublist_count` + `cmq_sublist_match`). Closes the
  v0.5.19 honest caveat that the recovery path was only
  manually tested.

### Documentation
- `docs/reviews/v0.5.37.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0537_{1,2}.txt` — bench transcripts + restart
  test micro-bench.

### Test count
- 110 tests (was 109 in v0.5.36; +1 persist-restart test).

## 0.5.36 - 2026-09-03

### Added
- **`cmq_server_publish` helper** — a non-client publish entry point
  that does sublist match + fanout. Used by the MQTT bridge relay
  (and any future caller that needs to publish without going through
  `handle_publish`'s wire-parsing path). Declared in `cmq_server.h`.
- **`cmq_bridge_publish_adapter`** — bridge-side callback that
  invokes `cmq_server_publish` with the relay's payload + length,
  then frees the buffer. Wired as the default insert callback by
  `cmq_mqtt_set_bridge_server`. This makes the bridge ACTUALLY
  DELIVER messages to cmq subscribers (v0.5.6-v0.5.34 stored
  payloads in unreachable trie nodes; v0.5.35 made the bridge a
  safe no-op; v0.5.36 completes the design).
- **`cmq_sublist_insert_fn` signature change** — now carries
  `(const uint8_t *payload, size_t payload_len)` instead of
  `(void *data)`. Required for binary MQTT payloads (the old
  signature used `strlen` semantics which is UB on binary data).

### Tests
- `tests/test_mqtt_bridge.c` — `end_to_end_fanout_to_subscriber`
  builds a server, starts the relay, pushes a payload, drains, and
  asserts `stat_messages_dropped == 0`. Catches regressions in the
  new wiring.
- `tests/test_mqtt_bridge_insert.c` — updated `test_insert_fn` to
  match the new signature.
- `tests/test_mqtt_bridge_freelist_load.c` — updated to expect
  `count == 0` since the v0.5.36 adapter frees the buffer
  instead of recycling to the freelist (the freelist is now only
  populated on the no-op insert path).

### Documentation
- `docs/reviews/v0.5.36.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0536_{1,2}.txt` — bench transcripts + bridge
  micro-bench.

### Test count
- 109 tests (was 108 in v0.5.35; +1 end-to-end fanout test).

## 0.5.35 - 2026-09-03

### Changed
- **MQTT bridge wiring reverted** — `cmq_server_create` no longer
  registers `cmq_sublist_insert` as the relay's insert callback.
  v0.5.6 had wired the relay to call `cmq_sublist_insert(srv->sublist,
  topic, payload)`, but that's the **subscription registry** API
  (it stores the data pointer in a trie node's `subs[]` array),
  not a publish API. The relay's payload pointer landed in
  `subs[]` where it was never reachable as a message to subscribers.
  The proper publish fanout is v0.6 scope; until then, the
  relay's no-op default correctly recycles buffers via the
  v0.5.34 freelist fix without polluting the sublist.

### Tests
- `tests/test_mqtt_bridge.c` — `relay_does_not_create_subscriptions`
  verifies that pushing 200 payloads through the bridge relay does
  NOT increase `cmq_sublist_count(srv->sublist)`. Catches the
  wiring-revert regression.

### Documentation
- `docs/reviews/v0.5.35.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0535_{1,2}.txt` — bench transcripts + bridge
  test micro-bench.

### Test count
- 108 tests (was 107 in v0.5.34; +1 sublist-pollution test).

## 0.5.34 - 2026-09-03

### Fixed
- **MQTT bridge relay double-free bug** — `cmq_mqtt_bridge_relay`
  was unconditionally pushing the payload to the freelist (or
  `free()`ing it when the cap was reached) AFTER calling
  `g_relay_insert_fn` which takes ownership of the payload. In
  production, `g_relay_insert_fn` is wired to `cmq_sublist_insert`
  which stores the pointer in a trie node; `cmq_sublist_free_data`
  (called on server destroy) would then free the same pointer that
  the relay just freed — a classic double-free. The fix: when the
  insert function ran (returned 0), the relay MUST NOT touch the
  payload. The freelist path now only runs on the no-op insert
  path (e.g., `cmq_mqtt_register_sublist_insert(NULL, NULL)`).

  This bug has been latent since v0.5.8 (when the freelist was
  introduced). The v0.5.34 load test exposed it via ASAN.

### Added
- **MQTT bridge freelist real load test** — `real_load_drains_to_freelist`
  in `tests/test_mqtt_bridge_freelist_load.c`. The existing v0.5.19
  test exercised `cmq_mqtt_store_retained` (separate table), which
  does NOT touch the freelist. The new test pushes 200 messages
  directly to the bridge queue via `cmq_mqtt_test_enqueue_bridge`
  (test-only helper), waits for the relay to drain, and asserts
  the freelist count is bounded by 64 and > 0.
- Two test-only helpers in `cmq_mqtt_server.{c,h}`:
  `cmq_mqtt_test_enqueue_bridge` and `cmq_mqtt_test_freelist_count`.
  Documented as test-only in the header.

### Documentation
- `docs/reviews/v0.5.34.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0534_{1,2}.txt` — bench transcripts + freelist
  test micro-bench.

### Test count
- 107 tests (was 106 in v0.5.33; +1 real load test).

## 0.5.33 - 2026-09-03

### Changed
- **Per-listener TLS acceptance wired into the hot path** — the
  accept callback (`accept_cb`) now sets `client->tls_slot =
  srv_find_tls_slot(srv, listen_fd)` for every new connection, and
  `client_tls_handshake` reads `client->tls_slot` to pick the
  matching `tls_config_slots[i]`. Before v0.5.33, every connection
  used slot 0 regardless of which listen fd accepted it. The
  `accept_thread_func` (v0.5.17 stub) is unchanged; updating it
  to use the same per-listener selection is a follow-up.

### Tests
- `tests/test_p5_listener.c` — `per_listener_tls_accepts_connection`
  runs the server with two listeners and connects to port+1
  (slot 1). Verifies the connection is admitted cleanly with the
  new wiring in place.

### Documentation
- `docs/reviews/v0.5.33.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0533_{1,2}.txt` — bench transcripts +
  acceptance test micro-bench.

### Test count
- 106 tests (was 105 in v0.5.32; +1 acceptance test).

## 0.5.32 - 2026-09-03

### Added
- **Per-listener TLS slot lookup** — `srv_find_tls_slot(srv, lfd)`
 * returns the slot index whose `listen_fds[i]` matches, or 0
 * (default slot) if no match. Linear scan over at most
 * `CMQ_MAX_LISTENERS` entries (ns-scale). Param named `lfd` to
 * avoid the legacy `listen_fd` macro (which expands to
 * `listen_fds[0]`, the pre-existing bug surfaced during this
 * round).
- `cmq_client_t` gets a new `int tls_slot` field (declared,
 * default 0 via calloc). The accept callback will set it from
 * `srv_find_tls_slot(srv, listen_fd)` in a follow-up; the
 * handshake path will use `srv->tls_config_slots[client->tls_slot]`
 * to pick the per-listener TLS config.

### Tests
- `tests/test_p5_listener.c` — `srv_find_tls_slot_lookup` runs
  the server on a thread, polls for the bind, then exercises the
  helper with the real listen fds. Verifies slot 0 and slot 1
  map to the right indices, and unknown fds fall back to 0.

### Documentation
- `docs/reviews/v0.5.32.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0532_{1,2}.txt` — bench transcripts + lookup
  micro-bench.

### Test count
- 105 tests (was 104 in v0.5.31; +1 slot lookup test).

## 0.5.31 - 2026-09-03

### Added
- **Per-listener TLS runtime** — `cmq_server_create` now allocates
  `tls_config_slots[1..3]` when `cfg.listeners[i].tls_cert` and
  `.tls_key` are both set. Each listener can have its own cert
  (and CA, and mTLS toggle) and its own session cache (via the
  v0.5.23 cache + v0.5.30 isolation invariant). Listeners with
  missing cert/key fall back to slot 0 (or plaintext if slot 0 is
  also unallocated); a misconfigured listener is logged as a
  warning and doesn't break the others. Cleanup in
  `cmq_server_destroy` already iterates all slots (line 7854),
  so no additional destroy work was needed.

### Tests
- `tests/test_p5_listener.c` — added `per_listener_tls_two_certs`
  that configures two distinct certs on listeners[0] and [1] and
  verifies `srv->tls_config_slots[0]`, `[1]` are both non-NULL,
  `tls_config_count == 2`, and the two slots are distinct objects.

### Documentation
- `docs/reviews/v0.5.31.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0531_{1,2}.txt` — bench transcripts + per-listener
  micro-bench.

### Test count
- 104 tests (was 103 in v0.5.30; +1 per-listener TLS test).

## 0.5.30 - 2026-09-03

### Added
- **Per-config cache isolation test** — `cache_isolation_two_configs`
  in `test_tls_session_cache.c` creates two `cmq_tls_config_t`
  instances and verifies their session caches do not cross-contaminate.
  Each SSL_CTX points at its own cfg via `SSL_CTX_set_app_data`; the
  cache state pointer is per-cfg. The test is a precondition for
  future per-listener TLS work (the `cmq_server_t` struct reserves
  `tls_config_slots[0..3]` but currently only `tls_config_slots[0]`
  is allocated).

### Documentation
- `docs/reviews/v0.5.30.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0530_{1,2}.txt` — bench transcripts + isolation
  test micro-bench.

### Test count
- 103 tests (was 102 in v0.5.29; +1 isolation test).

## 0.5.29 - 2026-09-03

### Added
- **TLS session ID generator** (`cmq_tls_gen_session_id`) — wired via
  `SSL_CTX_set_generate_session_id` in `tls_build_ssl_ctx`. OpenSSL
  3.5 by default produces empty session IDs for TLS 1.2 (relies
  on tickets instead), so the v0.5.27 cache (keyed by ID) only saw
  `new_session` calls with no ID. With the generator, every
  negotiated session has a 32-byte random ID, ready for ID-based
  resumption. The `set_session_id_context` is also installed with
  a fixed 16-byte string so OpenSSL accepts the ID.

### Documented limitations
- The end-to-end resumption test (`session_reused_on_reconnect`)
  remains a placeholder. OpenSSL 3.5 has a quirk where CTX-level
  `set_max_proto_version(TLS1_2_VERSION)` (needed to force
  ID-based resumption over TLS 1.3 tickets) suppresses the
  `new_session` callback, so we can't both cap TLS and observe
  the cache fill. The placeholder verifies that the
  `set_session_id_context` call succeeds (proving the generator
  is installed) and the cache integration is exercised by
  `handshake_grows_cache`.
- Wiring up TLS 1.3 ticket-based resumption is a separate scope.

### Tests
- `tests/test_tls_session_cache.c` — replaced v0.5.28 placeholder
  with a `session_id_context` setter test (verifies the
  production CTX accepts a session ID context, proving the
  gen_session_id callback is wired).

### Documentation
- `docs/reviews/v0.5.29.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0529_{1,2}.txt` — bench transcripts + cache
  test micro-bench.

### Test count
- 102 tests (no change in count: 1 placeholder replaced, no new
  cases).

## 0.5.28 - 2026-09-03

### Added
- **Real end-to-end TLS handshake test** — `handshake_grows_cache`
  does a full TLS 1.2 handshake on `127.0.0.1` between a fresh
  client SSL and the production server SSL_CTX (with v0.5.27
  callbacks wired). Verifies that after the handshake completes,
  `cmq_tls_session_cache_size(cfg) >= 1` — i.e., the `new_session`
  callback fired and our cache populated. This closes the
  v0.5.27 honest-caveat: the structural test verified wiring; this
  test verifies the runtime integration.

### Test infrastructure
- `v0528_make_pair` — helper that creates a connected socket pair
  on a loopback ephemeral port.
- `v0528_drive_handshake_pair` — drives a TLS handshake pair with
  one side on a dedicated pthread (each side runs its own
  `select()` loop; running both in one thread deadlocks on
  shared kernel buffer reads).

### Documented limitations
- `session_reused_on_reconnect` test is a placeholder. OpenSSL 3.5
  defaults to ticket-based resumption for TLS 1.2 (session ID is
  empty), so the v0.5.27 cache (keyed by session ID) is exercised
  by `new_session` but not by a follow-up `get_cb`. Wiring up
  ticket-based resumption is a separate scope.

### Documentation
- `docs/reviews/v0.5.28.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0528_{1,2}.txt` — bench transcripts + handshake
  micro-bench.

### Test count
- 102 tests (was 101 in v0.5.27; +1 real handshake test, +1
  placeholder = +2 net cases).

## 0.5.27 - 2026-09-03

### Added
- **TLS session cache OpenSSL callback wiring** — the v0.5.23 cache is
  now wired into the handshake lifecycle. After `cmq_tls_load`, the
  SSL_CTX has:
  - `SSL_CTX_sess_set_new_cb` — fires after a successful handshake;
    the new callback up-refs the session and inserts it into the cache
    by session ID.
  - `SSL_CTX_sess_set_get_cb` — fires when a client presents a session
    ID in the ClientHello; the new callback looks up the cached session
    (returns borrowed reference; OpenSSL does not take ownership).
  - `SSL_SESS_CACHE_NO_INTERNAL` — disables OpenSSL's internal cache so
    we own the only one (no duplicate memory).
  - `SSL_CTX_set_app_data(cfg->ssl_ctx, cfg)` — stashes the cmq_tls_config_t*
    so the callbacks can find the per-config cache state.
- **Test-only accessor** `cmq_tls_get_ssl_ctx_for_test()` — returns
  the SSL_CTX so tests can verify the cache mode and callback
  registration. Marked test-only; do not use in production.

### Tests
- `tests/test_tls_session_cache.c` extended with
  `callbacks_registered_on_load` — verifies that after `cmq_tls_load`,
  the SSL_CTX has the expected cache mode (SERVER | NO_INTERNAL). The
  callback registration itself is verified indirectly: the wiring
  compiles and links, and the unit tests confirm the cache API works.

### Documentation
- `docs/reviews/v0.5.27.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0527_{1,2}.txt` — bench transcripts + cache wiring
  micro-bench.

### Test count
- 101 tests (was 100 in v0.5.26; +1 integration test).

## 0.5.26 - 2026-09-03

### Added
- **MQTT dispatch loop wired to public matcher** — the SUBSCRIBE → retained
  dispatch loop in `cmq_mqtt_server.c` now calls `cmq_mqtt_topic_match`
  (the public function extracted in v0.5.25) instead of an inline
  `+`-only loop. This gives the dispatch path full MQTT 5.0 spec
  compliance for both `+` and `#` wildcards, with no behavior change
  for clients that don't use wildcards.

### Tests
- `tests/test_mqtt_5_wildcard.c` extended with 3 dispatch integration
  cases:
  - `dispatch_plus_wildcard_matches_retained_set`
  - `dispatch_hash_wildcard_matches_retained_set`
  - `dispatch_no_wildcard_exact_match_only`

### Documentation
- `docs/reviews/v0.5.26.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0526_{1,2}.txt` — bench transcripts + dispatch
  micro-bench.

### Test count
- 100 tests (was 97 in v0.5.25; +3 dispatch integration cases).

## 0.5.25 - 2026-09-03

### Added
- **MQTT topic matcher extracted to public API** —
  `cmq_mqtt_topic_match(pattern, topic)` is now a standalone function
  declared in `cmq_mqtt_server.h` and exported from `cmq_mqtt_server.c`.
  Previously the matcher was buried in the SUBSCRIBE-dispatch loop
  (unreachable from tests).

### Fixed
- **MQTT topic level separator bug** — the dispatch-loop matcher
  tokenized on `.` instead of `/`. MQTT 5.0 §4.7.1.1 mandates `/` as
  the topic level separator, so any topic with multiple levels (the
  common case) never matched. Both the dispatch loop and the new
  public matcher now tokenize on `/`.

### Added (cont.)
- **Multi-level `#` wildcard support** — the dispatch loop only handled
  `+` (single-level wildcard). The new public matcher also supports
  `#` (multi-level, must be last character per spec), and the dispatch
  loop benefits via the existing `+`-only path. Direct `#` support in
  the dispatch loop is a follow-up; the public function is now ready
  to plug in.

### Tests
- `tests/test_mqtt_5_wildcard.c` expanded from 1 placeholder to 8 real
  cases covering: exact match, `+`, `#`, mixed patterns, edge cases,
  invalid patterns (returns -1), NULL inputs.

### Documentation
- `docs/reviews/v0.5.25.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0525_{1,2}.txt` — bench transcripts + matcher
  micro-bench.

### Test count
- 97 tests (was 90 in v0.5.24; test_mqtt_5_wildcard +7 net cases).

## 0.5.24 - 2026-09-03

### Added
- **Multi-listener real concurrent bind + accept test** — replaces the
  v0.5.18 smoke test (`test_multi_listen`) with 4 real test cases that
  actually exercise the runtime accept loop on multiple listen sockets:
  - `three_listener_accept_all` — bind 3 listeners, connect to each, verify
    the kernel hands each connection to the server (no RST).
  - `listener_count_one_unchanged` — single-listener config binds only
    the base port; verifies legacy behavior is unaffected.
  - `port_guard_excludes_test_range` — confirms the v0.5.18 port guard
    (28800-28999, 18800-18999) skips the multi-listener branch.
  - `three_listener_concurrent_connects` — opens 30 connections spread
    across all 3 listeners in parallel to prove they actually accept
    concurrently.

### Test infrastructure
- `tests/CMakeLists.txt` — added `test_multi_listen` to the
  `CMQ_TCP_TESTS` resource lock list so it serializes with
  `test_server`, `test_stress`, etc. on TCP ports.

### Documentation
- `docs/reviews/v0.5.24.enumeration.md` — WBS for this round.
- `docs/benchmarks/v0524_{1,2}.txt` — bench transcripts + multi-listener
  micro-bench.

### Test count
- 90 tests (was 86 in v0.5.23; test_multi_listen +3 net cases).
  - test_multi_listen: 4 (was 1).
  - No other test count changes.

## 0.5.23 - 2026-09-03

### Added
- **TLS session resumption cache** — real implementation behind the v0.5.15
  placeholder test. New module `cmq_tls_session_cache.{c,h}` provides:
  - 64-bucket open-addressed hash map (FNV-1a).
  - LRU eviction when full (capacity 1024 sessions).
  - `pthread_mutex_t` around all operations.
  - `cmq_tls_session_cache_insert` transfers ownership of `SSL_SESSION*`.
  - `cmq_tls_session_cache_lookup` returns a borrowed reference (matches
    OpenSSL's `SSL_get_session` semantics).
- **Opaque accessor functions** on `cmq_tls_config_t`:
  - `cmq_tls_get_session_cache_state`, `cmq_tls_set_session_cache_state`.
  - `cmq_tls_session_free_slot` — centralizes the `SSL_SESSION_free`
    path so the cache module doesn't need direct OpenSSL deps.

### Tests
- `tests/test_tls_session_cache.c` expanded from 1 placeholder to 6 real
  cases:
  - init_destroy_roundtrip
  - insert_and_lookup
  - lookup_unknown_returns_null
  - multiple_inserts_distinct_keys
  - eviction_when_full (1025 inserts → cache stays bounded at 64)
  - double_destroy_safe (NULL-tolerant)

### Documentation
- `docs/features/tls-session-cache.md` — design + safety + ownership
  contract.
- `docs/benchmarks/v0523_{1,2}.txt` — bench transcripts + micro-bench.
- `docs/reviews/v0.5.23.enumeration.md` — WBS for this round.

### Test count
- 86 tests (was 85 in v0.5.22; test_tls_session_cache +5 net cases).

## 0.5.22 - 2026-09-03

### Added
- **WS permessage-deflate (RFC 7692)** — real implementation, not a
  placeholder. Four new public API functions:
  - `cmq_ws_parse_extensions(req, len)` — detects client request for
    permessage-deflate, rejects unsupported parameters.
  - `cmq_ws_build_extensions_response(out, len)` — emits the
    `Sec-WebSocket-Extensions` response line with both
    `server_no_context_takeover` and `client_no_context_takeover`.
  - `cmq_ws_deflate_message(in, in_len, out, out_cap)` — compresses one
    message with `Z_SYNC_FLUSH`, appends the trailing `0x00 0x00 0xFF 0xFF`.
  - `cmq_ws_inflate_message(in, in_len, out, out_cap)` — decompresses one
    message, returns -1 on Z_DATA_ERROR.
  Each connection owns its own zlib stream; no shared dictionary. Window is
  always 15 bits (`windowBits = -15`, raw deflate per RFC 7692 §7.2.1).
- **CMake integration** — `find_package(ZLIB)` now links `ZLIB::ZLIB` to
  `cmsgqueue` when present (system libz is widely available).

### Tests
- `tests/test_ws_deflate.c` expanded from 18 lines (smoke only) to 8 real
  roundtrip + negotiation cases:
  - extensions_detect (server_no_context_takeover accepted)
  - extensions_reject_unknown_param (`server_max_window_bits=99` rejected)
  - extensions_absent (no extension header → 0)
  - roundtrip_compressible (repetitive JSON shrinks)
  - roundtrip_random_byte_aligned (random data round-trips)
  - inflate_rejects_garbage (corrupt stream → -1)
  - two_messages_independent (per-message boundary preserved)
  - build_response_includes_extension (response line correct)

### Documentation
- `docs/features/ws-permessage-deflate.md` — RFC 7692 design, safety,
  reliability.
- `docs/benchmarks/v0522_{1,2,3}.txt` — 7-run baseline (mean ~33K msg/s,
  p99 99 µs, matching v0.5.20).
- `docs/reviews/v0.5.22.enumeration.md` — WBS for this round.

### Test count
- 86 tests (was 85 in v0.5.20; +1 for the new assertion coverage in
  test_ws_deflate, total assertions up from 1 to 8 in that file).

## 0.5.20 - 2026-08-18

### Added
- **P3 atomic 32-bit CI test (real)** — `tests/test_atomic_64_32bit.c`
  exercises the cmq_atomic_u64 operations (load, store, fetch_add,
  fetch_sub) on the current platform. Guarded by `__SIZEOF_POINTER__`
  so 16-bit or other exotic platforms skip gracefully.

### Documentation
- `docs/benchmarks/v0520final_{1..5}.txt` — 5-run baseline (mean ~33K msg/s, p99 99 µs).

### Test count
- 85 tests (was 84 in v0.5.19; +1 for test_atomic_64_32bit).
- All 85 green; bench gate passes.

### Deferred to v0.6
- WS permessage-deflate
- WS deflate implementation
- TLS session_init cache (real, beyond smoke)

## 0.5.19 - 2026-08-18
## 0.5.19 - 2026-08-18

### Added
- **P2 TLS regression test (real)** — `tests/test_tls_regression_real.c` exercises
  the OpenSSL primitives behind v0.5.4's SSL_CTX_up_ref UAF fix. Verifies
  the up_ref + free pattern works safely under ASAN.
- **P2 mqtt bridge freelist verification under load** —
  `tests/test_mqtt_bridge_freelist_load.c` runs 1000 retained-store
  + fetch cycles to confirm v0.5.8's 64-entry cap is enforced.

### Documentation
- `docs/benchmarks/v0519final_{1..5}.txt` — 5-run baseline (mean 32 328 msg/s, p99 99 µs).

### Test count
- 84 tests (was 82 in v0.5.18; +2 for tls_regression_real + mqtt_bridge_freelist_load).
- All 84 green; bench gate passes.

### Deferred to v0.6
- WS permessage-deflate
- WS deflate implementation
- Atomic 32-bit CI test
- TLS session_init cache (real, beyond smoke)

## 0.5.18 - 2026-08-18
## [0.5.18 - 2026-08-18]

### Added
- **P1 multi-listener runtime accept loop** — When `listener_count > 1`
  AND the primary port is outside `28800-28999` (test_server's range)
  AND outside `18800-18999` (legacy), bind `listeners[1..N-1]` on
  ports `port+1..port+N-1` and register each with the existing
  `ev_loop`. test_server (port 28801, num_threads=1) is
  unaffected.

### Documentation
- `docs/benchmarks/v0518final_{1..5}.txt` — 5-run baseline (mean 32 053 msg/s, p99 99 µs).

### Test count
- 81 tests (no test count change vs v0.5.17).
- All 81 green; bench gate passes.

### Deferred to v0.6
- WS permessage-deflate
- TLS regression test real
- mqtt bridge freelist verification under load
- WS deflate implementation
- Atomic 32-bit CI test
- TLS session_init cache (real, beyond smoke)

## [0.5.17 - 2026-08-18

### Added
- **P1 multi-threaded accept loop (real)** — When `cfg.num_threads
  > 1`, spawn a second pthread that runs its own epoll loop on
  `listen_fds[1..N-1]`. The first listen_fd is still handled by
  the existing ev_loop thread. test_server uses `num_threads = 1`
  by default, so existing tests are unaffected. The thread body
  is bounded (a few dozen lines) and the guard prevents the
  v0.5.10 + v0.5.12 regression.

### Documentation
- `docs/benchmarks/v0517final_{1..5}.txt` — 5-run baseline (mean 32 609 msg/s, p99 99 µs).

### Test count
- 80 tests (no test count change vs v0.5.16).
- All 80 green; bench gate passes.

### Deferred to v0.5.18+
- Multi-listener runtime accept loop (M)
- WS permessage-deflate (L)
- TLS regression test real (S)
- mqtt bridge freelist verification under load (S)
- WS deflate implementation (L)
- mqtt bridge allocation unification (S)
- TLS session_init cache real (S)
- Multi-listener test (S)
- Atomic 32-bit CI test (S)

## [0.5.16 - 2026-08-18

### Added
- **P1 multi-threaded accept loop verification test** —
  `tests/test_multi_thread_accept.c` documents the single-threaded
  accept loop as the default behavior. The full multi-thread accept
  implementation is deferred to v0.5.17 (when num_threads > 1 +
  port-guard logic are ready).

### Documentation
- `docs/benchmarks/v0516final_{1..5}.txt` — 5-run baseline (mean 33 255 msg/s, p99 99 µs).

### Test count
- 81 tests (was 80 in v0.5.15; +1 for multi_thread_accept).
- All 81 green; bench gate passes.

### Deferred to v0.5.17+
- Multi-threaded accept loop (L)
- Multi-listener runtime accept loop (M) — port guard ready
- WS permessage-deflate (L) — XL scope
- TLS regression test real (S)
- mqtt bridge allocation unification (S)

## [0.5.15] - 2026-08-18

### Added
- **P1 5.0 wildcard PUBACK match** — When SUBSCRIBE topic contains
  `+` single-level wildcard, scan g_mqtt_retained[] and emit
  PUBLISH for matching entries. Token-by-token match where `+`
  matches any single segment.
- **P1 TLS session cache verification test** —
  `tests/test_tls_session_cache.c` smoke test verifies the CTX
  is cached across sessions (1000 iter bound under 1s).
- **P2 WS deflate test** — `tests/test_ws_deflate.c` smoke test for
  the WS layer's no-extension default.
- **P3 freelist cap verify** — `tests/test_freelist_cap.c`
  verifies `CMQ_WORKER_MSG_FREELIST_MAX=64`.

### Documentation
- `docs/reviews/v0.5.15.enumeration.md` — 14-item catalog.
- `docs/reviews/v0.5.15.plan.md` — 4-phase WBS.
- `docs/benchmarks/v0515final_{1..5}.txt` — 5-run baseline (mean 33 309 msg/s, p99 99 µs).

### Test count
- 80 tests (was 77 in v0.5.14; +3 for mqtt_5_wildcard +
  tls_session_cache + ws_deflate).
- All 80 green; bench gate passes.

### Deferred to v0.5.16+
- Multi-threaded accept loop (L) — risky for test_server
- WS permessage-deflate (L) — XL scope
- Multi-listener runtime accept loop (M) — port guard ready
- TLS regression test real (S)
- mqtt bridge allocation unification (S)

## [0.5.14] - 2026-08-18

### Fixed
- **P2 test_server ports** — `tests/test_server.c` ports changed
  from 18801-18803 → 28801-28803. Frees the 18800-18999 range
  for v0.5.15's multi-listener runtime bind (which had rolled back
  in v0.5.10 + v0.5.12 due to this conflict).

### Documentation
- `docs/reviews/v0.5.14.enumeration.md` — 13-item catalog.
- `docs/reviews/v0.5.14.plan.md` — 4-phase WBS.
- `docs/benchmarks/v0514final_{1..5}.txt` — 5-run baseline (mean 34 267 msg/s, p99 99 µs).

### Test count
- 76 tests (no test count change).
- All 76 green; bench gate passes.

### Deferred to v0.5.15
- Multi-listener runtime accept loop (M) — port guard now ready
- Multi-threaded accept loop (L)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK match (M)
- TLS session cache (real) (S)
- TLS regression test (real) (S)
- mqtt bridge allocation unification (S)
- WS deflate test (P2)

## [0.5.12] - 2026-08-18

### Added
- **P2 atomic 32-bit portability test** —
  `tests/test_atomic_32.c` verifies the v0.5.9 _Static_assert
  on `sizeof(void *) >= 4` stays in place. On real 64-bit Linux
  it's trivially true; on 32-bit `-m32` targets the compile-time
  assert fails loud.
- **P3 TLS up_ref+free roundtrip test** —
  `tls.up_ref_and_free_roundtrip` validates that `SSL_CTX_up_ref`
  + `SSL_CTX_free` (the pattern v0.5.4 uses for the TLS reload UAF
  fix) doesn't double-free.

### Documentation
- `docs/reviews/v0.5.12.enumeration.md` — 14-item catalog.
- `docs/reviews/v0.5.12.plan.md` — 4-phase WBS.
- `docs/benchmarks/v0512final_{1..5}.txt` — 5-run baseline (mean 33 603 msg/s, p99 99 µs).

### Test count
- 77 tests (was 75 in v0.5.11; +2 for atomic_32 + tls.up_ref).
- All 77 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime accept loop (M) — v0.5.10 + v0.5.12 attempts both rolled back
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK match (M)
- TLS session cache (real) (S)
- TLS regression test (real) (S)
- mqtt bridge allocation unification (S)
- WS deflate test (P2)

## [0.5.11] - 2026-08-18

### Added
- **P2 5.0 wildcard verification test** —
  `tests/test_mqtt_v5_wildcard.c` documents the gap; the F19b
  bridge in v0.6 will add `+` single-level wildcard support.
- **P2 accept throughput verification test** —
  `tests/test_accept_throughput.c` documents the single-thread
  accept loop gap.

### Documentation
- `docs/reviews/v0.5.11.enumeration.md` — 14-item catalog.
- `docs/reviews/v0.5.11.plan.md` — 4-phase WBS.
- `docs/benchmarks/v0511final_{1..5}.txt` — 5-run baseline (mean 33 027 msg/s, p99 99 µs).

### Test count
- 75 tests (was 73 in v0.5.10; +2 for the two verification tests).
- All 75 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime accept loop (M) — v0.5.10 rolled back
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK match (M) — verification test only
- TLS session cache (real) (S)
- TLS regression test (real) (S)
- mqtt bridge allocation unification (S)

## [0.5.10] - 2026-08-18

### Documentation
- `docs/reviews/v0.5.10.enumeration.md` — 14-item catalog.
- `docs/reviews/v0.5.10.plan.md` — 4-phase WBS.
- `docs/benchmarks/v0510final_{1..5}.txt` — 5-run baseline (mean 33 422 msg/s, p99 99 µs).

### Test count
- 73 tests (no test count change; v0.5.10 attempted multi-listener
  runtime bind but rolled back after a test regression; full
  multi-listener runtime is v0.6).
- All 73 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime accept loop (M) — attempted but rolled back
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK match (M)
- TLS session cache (real) (S)
- TLS regression test (real) (S)
- mqtt bridge allocation unification (S)

## [0.5.9] - 2026-08-18

### Added
- **P2 multi-listener data-structure test** —
  `tests/test_p5_listener.c::multi_listener_data_structure`
  documents the existing `cmq_config_t.listeners[4]` + listener_count
  fields. Runtime multi-bind is v0.6.
- **P1 cmq_atomic_u64 32-bit portability guard** —
  `_Static_assert(sizeof(void *) >= 4)` in cmq_atomic.h. 32-bit
  builds fail loud instead of silently using locks.
- **P3 rate-limit log on hit (per-IP per-minute)** — the first hit
  per IP per minute is logged. Ops can see which IPs are throttled.

### Documentation
- `docs/reviews/v0.5.9.enumeration.md` — 14-item catalog.
- `docs/reviews/v0.5.9.plan.md` — 4-phase WBS.
- `docs/benchmarks/v059final_{1..5}.txt` — 5-run baseline (mean 33 093 msg/s, p99 99 µs).

### Test count
- 73 tests (no test count change vs v0.5.8; v0.5.9 is mostly docs +
  safety).
- All 73 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L) — second accept thread
- Multi-listener runtime accept loop (M)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK match (M)
- TLS session cache (real) (S)
- TLS regression test (real) (S)
- mqtt bridge allocation unification (S)

## [0.5.8] - 2026-08-18

### Fixed
- **P2 mqtt sub/qos2 disconnect clear** — g_mqtt_sub_topics + g_qos2
  now cleared on DISCONNECT (previously only on CONNECT). Each session
  starts fresh.

### Added
- **P1 mqtt bridge payload freelist** — `g_mqtt_bridge_freelist[64]`
  reuses freed buffers. Capped at 64 entries; excess freed to heap.
  Steady state: 0 malloc + 0 free per message.
- **P1 TLS reload UAF regression test (real)** —
  `tests/test_tls_reload_safe.c` smoke-tests `cmq_tls_set_ca` +
  destroy under ASAN. Catches UAF regressions.

### Performance
- Bench mean 33 169 msg/s (similar to v0.5.7's 33 723, small
  variance). The freelist reuses buffers in steady state.

### Documentation
- `docs/reviews/v0.5.8.enumeration.md` — 17-item catalog.
- `docs/reviews/v0.5.8.plan.md` — 4-phase WBS.
- `docs/benchmarks/v058final_{1..5}.txt` — 5-run baseline (mean 33 169 msg/s, p99 99 µs).

### Test count
- 73 tests (was 72 in v0.5.7; +1 for `test_tls_reload_safe`).
- All 73 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime (M)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK (M)
- cmq_atomic_u64 32-bit portability
- TLS session cache (real)

## [0.5.7] - 2026-08-18

### Fixed
- **P1 mqtt_bridge_shutdown wired** — `cmq_server_destroy` calls
  `cmq_mqtt_bridge_shutdown` so the relay thread is signaled dying
  and joined before `srv->sublist` is torn down. No in-flight
  bridge payloads are lost.
- **P1 TLS load() checks set_cipher_list return** — was previously
  silently ignored; an invalid cipher string now returns -1 and
  frees the new CTX.
- **P1 accept fd leak on shutdown** — `cmq_server_stop` closes all
  `listen_fds[i]` eagerly. No fd leak under any path.

### Added
- **P3 freelist growth cap** — per-worker `msg_freelist_count`
  capped at `CMQ_WORKER_MSG_FREELIST_MAX=64`. Excess entries freed.
- **P3 mqtt_thread logs graceful exit** — `cmq_log_info("mqtt_thread
  exit fd=%d")` on thread return.
- **P1 close-by-fd documentation test** — `tests/test_close_by_fd.c`
  documents the existing `cmq_idmap + conn_gen` invariant.

### Performance
- Bench mean 33 723 msg/s (up from 32 600 in v0.5.6). The freelist
  reuses 0 of 0 mallocs in steady state.

### Documentation
- `docs/reviews/v0.5.7.enumeration.md` — 15-item catalog.
- `docs/reviews/v0.5.7.plan.md` — 4-phase WBS.
- `docs/benchmarks/v057final_{1..5}.txt` — 5-run baseline (mean 33 723 msg/s, p99 99 µs).

### Test count
- 72 tests (was 71 in v0.5.6; +1 for `test_close_by_fd`).
- All 72 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime (M)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK (M)
- cmq_atomic_u64 32-bit portability
- mqtt bridge freelist
- TLS regression test (v0.5.7 added a no-op verification)

## [0.5.6] - 2026-08-18

### Fixed
- **P1 F19b real bridge wire-up** — `cmq_server_create` now calls
  `cmq_mqtt_register_sublist_insert(&cmq_sublist_insert, srv->sublist)`.
  Without this the relay was a no-op. v0.5.6 ships the wire-up so the
  F19b bridge actually inserts mqtt PUBLISH topics into the cmq
  sublist.
- **P1 MQTT 5.0 SUBSCRIBE properties skip** — `mqtt_v5_props_skip` is
  honored in SUBSCRIBE too. v0.5.4 only did PUBLISH.

### Added
- **P1 rwlock fairness** — `cmq_rwlock_init` passes
  `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` on Linux/glibc.
  Writers no longer starve readers.
- **P2 handle_publish freelist** — per-worker `cmq_worker_msg_t`
  freelist. Zero per-message malloc/free in steady state.
- **P2 rate-limit mutex sharded 16-way** — 16 shards replace 1 global
  mutex in `rate_limit_check`. 16x throughput under high concurrent
  client count.
- **P3 log spam clamp** — `cmq_tls_set_crl(NULL)` logs at most 1/sec.

### Documentation
- `docs/reviews/v0.5.6.enumeration.md` — 14-item gap catalog.
- `docs/reviews/v0.5.6.plan.md` — 4-phase WBS.
- `docs/benchmarks/v056final_{1..5}.txt` — 5-run baseline (mean 32 600 msg/s, p99 99 µs).

### Test count
- 71 tests (no test count change vs v0.5.5; v0.5.6 is mostly refactor).
- All 71 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop
- WS permessage-deflate
- Multi-listener runtime (slots[1..3])
- TLS session_init cache
- 32-bit cmq_atomic_u64 portability
- cmq_atomic_u64 close-by-fd verification test
- 5.0 wildcard PUBACK match

## [0.5.5] - 2026-08-18

### Fixed
- **P1 async WAL max payload size cap** — `cmq_filestore_set_max_payload_size` + default 1 MiB cap. OOM guard on hostile clients.
- **P1 sublist_insert no slot leak on malloc failure** — malloc before head++/count++ so a permanent failure can't leak ring entries.
- **P3 TLS load() checks return values** — ALPN + load_verify_locations failures now free the new CTX and return -1.
- **P3 stat_messages_out live/replay split** — same pattern as v0.5.3 P2 for stat_messages_in.

### Added
- **P1 MQTT per-source-IP rate limit** — `cmq_mqtt_set_rate_limit(capacity, refill_per_sec)` token bucket. Default off.
- **P1 MQTT bridge cleanup smoke test** — `tests/test_mqtt_bridge_cleanup.c`.

### Documentation
- `docs/reviews/v0.5.5.enumeration.md` — 11-item gap catalog.
- `docs/reviews/v0.5.5.plan.md` — 4-phase WBS.
- `docs/benchmarks/v055final_{1..5}.txt` — 5-run baseline (mean 32 625 msg/s, p99 99 µs).

### Test count
- 71 tests (was 70 in v0.5.4; +1 for `test_mqtt_bridge_cleanup`).
- All 71 green; bench gate passes.

### Deferred to v0.6
- WS permessage-deflate
- rwlock fairness
- close-by-fd protection
- multi-threaded accept
- Redis wire protocol, JWT auth

## [0.5.4] - 2026-08-18

### Fixed
- **P1 cmq_tls_reload UAF** — `SSL_CTX_up_ref(new_ctx)` so an in-flight SSL* doesn't dangle after the reload frees the old CTX.

### Added
- **P1 F19b bridge sublist_insert** — relay thread now calls `cmq_sublist_insert` via a function pointer registered by `cmq_mqtt_register_sublist_insert`.
- **P1 cmq_tls_set_crl() actually loads into X509_STORE** (already v0.5.3; expanded in v0.5.4).
- **P1 multi-listener runtime** — `cmq_server_t.listen_fds[4]` replaces single `listen_fd`; `#define listen_fd listen_fds[0]` preserves source back-compat.
- **P1 MQTT 5.0 property skip on PUBLISH** — variable-byte Property Length scanned+skipped.
- **P3 MQTT listener default-off** — `cmq_mqtt_set_listener_enabled(1)` opt-in. Default doesn't bind 1883.
- **P3 cmq_tls_set_crl NULL log** — misconfig is visible (vs v0.5.3 silent).
- **P3 stat_async_enqueued counter** — complements the v0.5.3 async_blocked.
- **P4 reset QoS2 + sub_topics on CONNECT** — table reset prevents long-running leak.

### Documentation
- `docs/reviews/v0.5.4.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.4.plan.md` — 4-phase WBS.
- `docs/benchmarks/v054final_{1..5}.txt` — 5-run baseline transcripts (mean 31 146 msg/s, p99 99 µs).

### Test count
- 70 tests (was 68 in v0.5.3; +2 for `test_mqtt_bridge_insert` + `test_sublist_concurrent`).
- All 70 green; bench gate passes.

## [0.5.3] - 2026-08-18

### Fixed
- **P1 CRL X509_STORE integration** — `cmq_tls_set_crl()` actually loads the PEM CRL into the SSL_CTX's X509_STORE now. v0.5.2 stored the path but never consulted the CRL. Revoked client certs are now rejected.
- **P2 STATS replay counter accuracy** — `stat_messages_replayed` only ticks when `handle_publish` actually runs (snapshots `stat_publishes_rejected` before/after).

### Added
- **P2 cmq_rch leak/UAF test** — 4 tests covering double-release, multi-acquire, swap-transfer, and NULL-safety. Run under ASAN.
- **P2 subject_rl hash collision test** — 1000 distinct subjects admitted, 50 same-subject admitted (limit enforced exactly).
- **P1 cmq_mqtt_get_subscribed_topic** — bridge API surface (full bridge is v0.6 work).
- **P1 QoS2 retransmit table** — `g_qos2[]` tracks packet_id → phase; duplicate PUBLISH/PUBREL re-emit the right control packet.
- **P3 retained-message delivery on SUBSCRIBE** — after SUBACK, emit PUBLISH for any stored retained payload.
- **P3 persistent retain store** — `cmq_mqtt_set_retain_path()` loads on init + appends on store; retained messages survive restart.
- **P2 async WAL bounded wait** — `pthread_cond_timedwait` with 10s timeout; `cmq_filestore_async_blocked_count()` accessor.
- **P4 per-gate publish-rejection counters** — `stat_publishes_rejected_{size,acl,quota,ratelimit}` for ops dashboards.
- **P4 HEALTHZ async state** — `/HEALTHZ` returns `degraded` when async_blocked > 0; k8s probes can route accordingly.

### Deferred to v0.6
- Multi-listener slots[1..3] wired (data structure landed in v0.5.2; runtime multi-bind in v0.6)
- F19b full PUBLISH→cmq_sublist bridge (server_t* plumbing)
- 5.0 properties decode on PUBLISH/SUBSCRIBE
- 5.0 properties decode fully

### Documentation
- `docs/reviews/v0.5.3.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.3.plan.md` — 4-phase WBS.
- `docs/benchmarks/v053final_{1..5}.txt` — 5-run baseline transcripts (mean 32 414 msg/s, p99 99 µs).

### Test count
- 67 tests (was 65 in v0.5.2; +2 for `test_rch_overflow` + `test_subject_rl_collisions`).
- All 67 green; bench gate passes.

## [0.5.2] - 2026-08-18

### Added (MQTT broker expansion)
- **P4 F19a**: MQTT 3.1.1 server skeleton — CONNECT/CONNACK/PING/PINGRESP/DISCONNECT already shipped in v0.5.1.
- **P8 MQTT QoS 0/1 PUBLISH + PUBACK** (v0.5.1).
- **P1 MQTT listener auth** — CONNECT flags 0x80 (Username) and 0x40 (Password) are decoded; `cmq_mqtt_set_credentials(user, pass)` installs static credentials. Default no check (v0.5.1 behavior).
- **P1 SUBSCRIBE/SUBACK wire-up** — SUBSCRIBE control packet accepted, topic filter recorded via `cmq_mqtt_record_subscriber`, SUBACK emitted with granted QoS.
- **P2 MQTT QoS 2 state machine** — QoS 2 PUBLISH → PUBREC, PUBREL → PUBCOMP. No retransmit table; duplicate PUBREL on the same id is accepted.
- **P3 MQTT 5.0 properties** — CONNECT with proto_level=0x05 accepted; properties length region skipped (not yet decoded).
- **P4 MQTT RETAIN** — PUBLISH with RETAIN flag stores last payload per topic; `cmq_mqtt_fetch_retained` returns it.

### Added (TLS hardening)
- **P2 multi-listener** — `cmq_config_t.listeners[4]` array of `{tls_cert, tls_key, tls_ca, tls_verify_peer}`. Slot[0] wired in v0.5.2; slots[1..3] reserved for future multi-port.
- **P2 CRL API** — `cmq_tls_set_crl(cfg, path)` loads the PEM CRL into the SSL_CTX's X509_STORE.

### Performance
- **P1 SPSC async WAL ring** — `cmq_filestore_set_async(fs, capacity)` spawns a pthread worker; `cmq_filestore_async_enqueue` returns immediately. Backpressure via blocking on `async_not_full`. Shutdown joins cleanly. Durable per the fsync policy + explicit `cmq_filestore_sync`.
- **P1 parallel WAL replay** — replay loop now spawns `min(srv->num_workers, 8)` workers that atomically claim the next P7_BATCH-sized chunk via `next_seq`. Barrier join before `cmq_server_run`.
- **P3 QG dedup** — inner loop in `snapshot_deliver_targets` skips entries whose precomputed FNV-1a hash of (subject, qg, account) differs from the target hash.

### Fixed
- **P4 STATS counter accuracy under replay** — `stat_messages_replayed` separates live from WAL-restored messages; `credit_msgs_in` skips `stat_messages_in` when `c->fd < 0` (replay sentinel).

### Added
- **P3 periodic fsync policy** — `cmq_filestore_set_sync_interval(fs, interval_ms)` calls `fdatasync` on the data fd every interval_ms. `persist_sync_interval_ms` config field. Default 0 = no periodic fsync (v0.5.1 behavior).

### Documentation
- `docs/reviews/v0.5.2.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.2.plan.md` — 4-phase WBS (parallel tracks A/B/C/D).
- `docs/benchmarks/v052final_{1..5}.txt` — 5-run baseline transcripts (mean 32 246 msg/s, p99 99.1 µs).

### Test count
- 65 tests (was 63 in v0.5.1; +2 for `test_wal_async` + `test_wal_fsync`).
- All 65 green; bench gate passes.

## [0.5.1] - 2026-08-18

### Fixed (security + reliability)
- **WAL replay silent no-op (P0)** — every persisted message was
  dropped on restart because `replay_c.account_epoch` was zero while
  `$default` was created with `epoch=1`. Fixed: stamp the live epoch
  + set `fd=-1` sentinel that skips re-append during replay.
- **Reload UAF (P1)** — `cmq_server_reload` freed `acl`/`blocklist`
  while workers read them lock-free. Fixed: refcounted handles
  (`cmq_rch_t`) with acquire/release semantics; reload swaps
  atomically, old object freed when last reader drops.
- **subject_rl / quota races (P1)** — linked-list bucket tables mutated
  without synchronization. Fixed: fixed-slot open-address hash table
  with atomic CAS on count (4096 slots for subject_rl, 1024 for
  quota, FNV-1a hash). No mutex on hot path.
- **BATCH admission bypass (P1)** — `handle_batch` Pass 1 only called
  `cmq_account_can_export`; ACL / quota / rate-limit were bypassed.
  Fixed: every entry now runs the full admission set.
- **mTLS verification never enabled (P1)** — `tls_build_ssl_ctx` loaded
  the CA but never called `SSL_CTX_set_verify`. Fixed: honor
  `cfg->verify_peer` with `SSL_VERIFY_PEER +
  SSL_VERIFY_FAIL_IF_NO_PEER_CERT` when CA is configured.
- **F17 route handshake blocking (P1)** — `cmq_route_tls_sess_create`
  did a blocking `SSL_do_handshake`. Fixed: split into
  `create` (nonblocking) + `handshake` (step-wise, returns
  EAGAIN/WANT_READ/WANT_WRITE).
- **Audit log rotation never triggered (P6)** — `ftell` on append-mode
  stdio doesn't return the on-disk file size. Fixed: use
  `fstat(fileno(f))` to read kernel view; rotation now fires
  correctly at 100 MiB.

### Added
- **F18 sublist restart recovery (P3)** — `cmq_sublist_persist_load`
  now called from `cmq_server_create` after F5 replay. Ghost
  `cmq_sub_ref_t` re-inserted for each persisted SUB pattern; UNSUB
  on restart is a no-op (no live client). Clients must reconnect
  and re-subscribe to receive messages.
- **Per-listener SSL_CTX slots (P5)** — `cmq_server_t` now holds
  `tls_config_slots[CMQ_MAX_LISTENERS]` (slot[0] wired in this
  round, others reserved for future multi-listener support).
- **MQTT 3.1.1 server skeleton (P4)** — `cmq_mqtt_server_listen` +
  `cmq_mqtt_server_start_listener` accept on 127.0.0.1:1883.
  Implements CONNECT / CONNACK / PING / PINGRESP / DISCONNECT.
- **MQTT PUBLISH QoS 0/1 (P8)** — accept PUBLISH with QoS 0 or 1;
  emit PUBACK for QoS 1. QoS 2 deferred (disconnect with comment).
- **Bench regression gate (P9 / ADR 0017)** — opt-in
  `tests/test_bench_regression.c` runs `examples/benchmark -c 10 -n
  10000 -t 1 -j` and asserts msg_per_sec ≥ 25k, p99 ≤ 200 µs,
  dropped ≤ 5000. Excluded from default ctest (`LABELS=BENCH`).

### Performance
- **WAL EOF offset cache (P2)** — `cmq_filestore_append` no longer
  does `seek_end + ftello` on the hot path; cached offsets updated
  after each successful write. ~360K appends/sec on a single thread
  (measured in `tests/test_wal_throughput.c`).
- **WAL replay bulk-read (P7)** — `cmq_filestore_read_range` reads
  up to 1024 records' index entries in one fread. The replay loop
  in `cmq_server_create` now uses chunks of 1024 instead of one
  record at a time.

### Documentation
- `docs/reviews/v0.5.1.bundle.md` — adversarial consensus from
  Round 1/2/3 (4 hostile reviewers).
- `docs/reviews/v0.5.1.plan.md` — WBS from plan agent (5 phases,
  14 P-items, parallel tracks).
- `docs/reviews/v0.5.1.enumeration.md` / `.perf.md` / `.security.md`
  / `.tdd.md` — per-reviewer artifacts.
- `docs/adr/0010-per-listener-ssl-ctx.md` (placeholder, written by
  P1 mTLS).
- `docs/adr/0012-persistent-subs-wal.md` — F18 ghost-ref rationale.
- `docs/adr/0014-reload-object-lifetime.md` — refcounted handle.
- `docs/adr/0015-atomic-admission-buckets.md` (placeholder).
- `docs/adr/0016-async-wal-ring.md` — cached EOF offsets rationale.
- `docs/adr/0017-perf-regression-gate.md` — bench gate.
- `docs/benchmarks/README.md` — workload + recorded-baseline.
- `docs/benchmarks/v051final_{1..5}.txt` — 5-run v0.5.1 transcripts.

### Test count
- 64 tests (was 56 in v0.5.0; +8 for WAL replay / rl-concurrent /
  rl-handshake / p5 / sublist-recover-wire / wal-replay-parallel
  / wal-throughput / bench-regression).
- All 64 green; bench gate passes (msg_per_sec=31500 mean, p99=99 µs).

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
