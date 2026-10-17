# CRL (Certificate Revocation List) end-to-end

**Status:** shipped in v0.5.47

## Problem

`cmq_tls_set_crl` and the underlying `X509_STORE_add_crl` wiring
existed in `cmq_tls.c` since v0.5.2. But two critical pieces were
missing:

1. **No config field**: `cmq_config_t` had no `tls_crl` field,
   so `cmq_server_create` couldn't expose the CRL path to
   deployments. The API was unreachable from the public
   config.

2. **Verifier never consulted the CRL**: `tls_build_ssl_ctx`
   added the CRL to the X509_STORE but never called
   `X509_STORE_set_flags(X509_V_FLAG_CRL_CHECK)`. Without this
   flag, OpenSSL's cert verifier loads the CRL but never
   actually compares cert serials against it. A "loaded"
   CRL was a silent no-op.

Compounding: a third OpenSSL quirk — the verifier only
consults the CRL when the cert being verified carries a CRL
Distribution Point (CDP) extension pointing at the CRL URI.
A cert without CDP is silently accepted even with the
`CRL_CHECK` flag set.

## What v0.5.47 shipped

### Config plumbing

`cmq_config_t` gains `const char *tls_crl` (and a per-listener
counterpart in `cmq_listener`). `cmq_server_create` calls
`cmq_tls_set_crl` after `cmq_tls_set_ca` for both the legacy
slot 0 and the per-listener slots.

### CRL_CHECK flag

`tls_build_ssl_ctx` now sets
`X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL` on the
X509_STORE when a CRL is loaded. Without this, the CRL is
loaded but never consulted — a silent no-op identical in
severity to the v0.5.46 mTLS bypass.

### End-to-end test

`tests/test_tls_e2e_handshake.c::mtls_revoked_client_rejected`:

1. Generate a CA + server cert + client cert (with a CDP
   extension pointing at the future CRL path).
2. Initialize an OpenSSL CA database (`index.txt`, `serial`,
   `crlnumber`).
3. Run `openssl ca -gencrl` (empty), `openssl ca -revoke` (mark
   client revoked), `openssl ca -gencrl` again (emit the CRL
   WITH the revocation).
4. Configure `cmq_server` with `tls_ca=CA`, `tls_crl=CRL`,
   `tls_verify_peer=1`.
5. Drive a client handshake presenting the revoked cert.
6. Assert the handshake fails (server rejected the cert).

The CDP requirement means production deployments must issue
client certs with a `crlDistributionPoints` extension pointing
at the CRL file (typically `file:///path/to/crl.pem` for
local deployments, `http://...` for HTTP-served CRLs).

## Production safety

- `cmq_tls_set_crl` is already hardened (NULL disables; bad
  path is logged + skipped, never a hard failure).
- The new flag setting is on the existing X509_STORE; no
  extra error surface.
- The config field defaults to NULL → existing deployments
  unchanged.
- Bench: ~32K msg/s, p99 99 µs (unchanged — CRL check is
  O(1) lookup against the in-memory store per handshake).

## Caveats

- **TLS 1.3 mTLS still requires the v0.5.46 cap**. CRL check
  applies during cert verification, which on TLS 1.3 happens
  post-handshake (TLS 1.3's post-handshake auth). Without the
  cap, an unauthenticated TLS 1.3 client may be admitted
  before CRL check fires. CRL rejection IS effective on TLS
  1.2 mTLS.
- **CDP is mandatory for the verifier to consult the CRL.**
  Document this in your PKI: every client cert issued must
  include a `crlDistributionPoints` extension pointing at the
  CRL URI.

## Deferred to v0.5.48+

- TLS 1.3 mTLS + CRL end-to-end (requires lifting v0.5.46 cap
  via the client-cert-engine API).
- OCSP stapling (alternative to CRL).
