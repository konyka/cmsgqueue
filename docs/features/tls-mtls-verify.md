# TLS 1.2 cap when mTLS verify_peer is enabled

**Status:** shipped in v0.5.46

## Problem

`cmq_tls_set_verify(cfg, 1)` correctly sets
`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` on the SSL_CTX.
But with OpenSSL 3.5 / TLS 1.3, this flag is silently a no-op:
the server never sends a `CertificateRequest`, so the client is
never asked for a cert, and any TLS 1.3 client is accepted as
authenticated regardless of whether it presented a cert.

The bug only manifests in TLS 1.3. TLS 1.2 honors the verify
flag and properly sends `CertificateRequest`; an empty
`Certificate` message from the client fails the handshake with
`handshake_failure` as RFC 5246 mandates.

## Fix

`tls_build_ssl_ctx` (cmq_tls.c) now caps the SSL_CTX at TLS 1.2
when `verify_peer` is set:

```c
if (cfg->verify_peer) {
    SSL_CTX_set_max_proto_version(cfg->ssl_ctx, TLS1_2_VERSION);
}
```

Plain TLS (no mTLS) is unaffected — TLS 1.3 remains the default.

## Why TLS 1.3 + verify_peer needs more work

TLS 1.3 moved client-cert verification out of the
`SSL_VERIFY_PEER` path. The server now needs to opt in via
`SSL_CTX_set_client_cert_engine` or a similar API to actually
send `CertificateRequest`. Without that, even with the verify
mode set, no client cert is requested.

Wiring the TLS 1.3 path is deferred to v0.5.47. Until then, the
TLS 1.2 cap keeps mTLS working.

## Test coverage

`tests/test_tls_e2e_handshake.c::mtls_required` verifies:

- Sub-test A: client WITH a CA-signed cert → handshake succeeds.
- Sub-test B: client WITHOUT a cert → handshake fails.

The test uses a self-signed CA that signs both the server cert
and a client cert, mirroring real CA deployments.

## Bench

~33K msg/s end-to-end, p99 99.0 µs — unchanged from v0.5.45.
