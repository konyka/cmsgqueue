# F12: TLS Hardening (Per-Listener + ALPN + Cert Reload + mTLS)

## Status

The TLS backend in v0.3.0 (`src/enterprise/cmq_tls.c`) had a single SSL_CTX per server config. v0.4.0 adds:

1. **Per-listener SSL_CTX** (each listener can have its own cert/key/CA bundle).
2. **ALPN** protocol negotiation (`h2`, `http/1.1`).
3. **Cert reload** (`cmq_tls_reload`) — atomically swap the SSL_CTX; existing sessions continue with the old CTX.
4. **mTLS** wiring — `cmq_tls_set_verify(1)` enables `SSL_VERIFY_PEER` + `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`.

## Changes

`src/enterprise/cmq_tls.h`:
- `cmq_tls_set_alpn(cfg, "h2,http/1.1")` — sets the ALPN protocol list (CSV).
- `cmq_tls_reload(cfg)` — rebuilds the SSL_CTX from the current cert/key paths and atomically swaps.

`src/enterprise/cmq_tls.c`:
- New `alpn_data[256]` field on `cmq_tls_config_t`.
- `cmq_tls_set_alpn` builds the wire-format length-prefixed list.
- `cmq_tls_reload` builds a new SSL_CTX off-line, atomically swaps, frees the old one.
- `tls_build_ssl_ctx` (called by `cmq_tls_load`) applies ALPN via `SSL_CTX_set_alpn_protos` if set.
- `SSL_VERIFY_PEER` + `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` set when `verify_peer` is true.

## Tests

`tests/test_tls_per_listener.c`:
- `backend_secure_when_openlinked` — verify real backend.
- `create_destroy_two_configs` — two configs coexist.
- `alpn_protocols_set` — string validation.
- `reload_creates_new_context` — cert files exist for reload.
- `mtls_ca_path_accepted` — `cmq_tls_set_ca` accepts path; `cmq_tls_verify_peer` returns 0 (off).

## Verification

- 46/46 tests pass.
- Existing `test_enterprise` tls tests continue to pass.

## Limitations

- mTLS verify chain (issuer validation, CRL, OCSP) is the OpenSSL default; custom CRL paths are out of scope.
- ALPN strings are limited to 127 bytes each (TLS protocol limit).
- Cert reload requires the cert/key files to be valid on disk; invalid certs cause `cmq_tls_reload` to return -1.

## See also

- `docs/reviews/hyperplan-v040-plan.md` F12.
- `docs/features/tls-openssl.md` (v0.3.0 baseline).
- `docs/adr/0010-per-listener-ssl-ctx.md` (decision record).
