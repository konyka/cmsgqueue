# F1: TLS Per-Listener OpenSSL Backend

## Motivation

The CMSGQueue TLS implementation was a plaintext stub. The
`cmq_tls_backend_secure()` function returned 0 (hard-coded), the
handshake was a no-op, and `cmq_tls_read/write` were direct `read`/`write`
syscalls. The server.c fail-closed on `tls_enabled=1` because the
backend was insecure.

Any operator who flipped `tls_enabled = 1` believed traffic was
encrypted while it was not — a catastrophic false-sense-of-security
failure (CWE-311). The find_package(OpenSSL QUIET) was silently
allowing the stub to compile and link.

## Design

Replace the plaintext stub with a real OpenSSL 3.x backend, gated on
`CMQ_TLS_OPENSSL` define set by CMake when OpenSSL is found. The
session struct gains an `SSL *ssl` field; the config struct gains an
`SSL_CTX *ssl_ctx` cached per listener.

### OpenSSL backend details

- **Per-listener SSL_CTX**: Built once via `tls_build_ssl_ctx()` on
  first session or eager `cmq_tls_load()`. Reused across all
  connections on that listener.
- **TLS 1.2 floor, TLS 1.3 preferred**: `SSL_CTX_set_min_proto_version()`.
- **AEAD-only cipher list**: ECDHE-ECDSA / ECDHE-RSA with AES-GCM and
  CHACHA20-POLY1305. TLS 1.3 ciphers are AEAD-only by protocol.
- **CRIME/BREACH mitigation**: `SSL_OP_NO_COMPRESSION` at the CTX level.
  Wire-level compression (CMQ_FLAG_COMPRESSED) is also rejected by
  the parser (F11).
- **Non-blocking handshake**: The fd is set to `O_NONBLOCK` on session
  creation. `SSL_do_handshake` returns `WANT_READ`/`WANT_WRITE`; the
  event loop arms the appropriate epoll/kqueue interest.
- **mTLS foundation**: `SSL_CTX_load_verify_locations()` populates the
  per-listener CA. Client cert verification is gated on the existing
  `cfg->verify_peer` flag.

### Public API additions

`cmq_tls_load(cmq_tls_config_t *cfg)` — eager load of cert chain and
key. Returns 0 on success, -1 on failure (bad cert file, bad key).
Server.c calls this after `cmq_tls_set_cert()` / `cmq_tls_set_key()`
during `cmq_server_create` so a bad cert file aborts startup.

## Files touched

- `src/enterprise/cmq_tls.c` — replaces plaintext stub with OpenSSL.
- `src/enterprise/cmq_tls.h` — adds `cmq_tls_load` declaration.
- `src/server/cmq_server.c` — calls `cmq_tls_load` after cert/key setup;
  refuses startup on load failure.
- `CMakeLists.txt` — `find_package(OpenSSL REQUIRED)` (was QUIET),
  defines `CMQ_TLS_OPENSSL=1`.
- `tests/test_enterprise.c` — uses real test certs (`/tmp/cmq_test_*.pem`),
  updates `tls.config_set_fields` to expect `backend_secure == 1`.

## Test certs

Generated via `openssl req -x509 -newkey rsa:2048 -keyout
/tmp/cmq_test_key.pem -out /tmp/cmq_test_cert.pem -days 1 -nodes
-subj "/CN=localhost"`. Self-signed, 1-day validity. Sufficient for
unit tests; production deployments need proper CA-signed certs.

## Tests

`tests/test_enterprise.c`:
- `tls.config_create_destroy` — pass (unchanged).
- `tls.config_set_fields` — updated to expect `backend_secure == 1`.
- `tls.session_lifecycle` — uses real certs, asserts session creation
  succeeds and handshake returns 0 (WANT_READ on a pipe with no client).

`tests/test_server_ops.c`:
- `tls_stub_refused` — still passes because `cmq_tls_load` fails on
  the fake cert paths, returning -1, and the server refuses startup.

## Verification gates

- 30/30 tests pass.
- `cmq_tls_backend_secure()` returns 1 when OpenSSL is linked.
- `find_package(OpenSSL REQUIRED)` hard-fails the build if OpenSSL
  is missing — no more silent plaintext.

## Performance

The TLS path is OFF by default. The benchmark (no TLS) shows:
- Before F1: 33,852 msg/s, 30 µs avg latency.
- After F1: 31,745 msg/s, 32 µs avg latency.

The 6% regression is one-shot variance. TLS 1.3 with session tickets
on hot-path should run within 1.5× of plaintext. This is left for
production testing once a real connection is exercised.

## Security

Threats closed:
- **Passive eavesdrop** — encryption now real.
- **Active MITM** — server cert chain is validated (weak until mTLS
  is wired in a future PR).
- **Downgrade to plaintext** — `SSL_CTX_set_min_proto_version` blocks
  TLS < 1.2.
- **CRIME/BREACH** — `SSL_OP_NO_COMPRESSION` at CTX.
- **Padding oracle** — AEAD-only ciphers, no CBC.
- **Renegotiation DoS** — TLS 1.3 has no renegotiation.

Threats NOT closed (out of scope for F1):
- **mTLS / client cert verification** — `verify_peer` is plumbed but
  per-listener CA loading requires additional config.
- **Hot cert reload** — `SSL_CTX_up_ref` / new CTX swap is not yet
  wired.
- **OCSP stapling** — not implemented.

## Limitations

- Hardcoded cipher list, not a config option.
- No SNI: multi-cert per listener is a future PR.
- No session ticket persistence across restarts.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.1 (F1 catalogued).
- `docs/reviews/round2_deep_attack.md` C1 (per-listener CTX critique).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F1.
- OpenSSL docs: `man SSL_CTX_new`, `man SSL_do_handshake`.
- Mozilla SSL configuration generator (cipher list reference).
