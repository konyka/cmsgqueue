# JWT / NKEY verify (v0.5.62, D3 phase 1)

CONNECT may present a compact HS256 JWT in the password
field when `jwt_hmac_secret` and `jwt_issuer` are set.
Verify uses OpenSSL HMAC. Tokens are never issued here.

## Checks

- `alg` must be `HS256` (fail closed on `none`).
- `iss` must equal `jwt_issuer`.
- `exp` required; reject if `now > exp + leeway`.
- `nbf` optional; reject if `now + leeway < nbf`.
- Default leeway is 60 seconds (`jwt_leeway_sec`).
- `sub`, when present, becomes the account name.

JWT mode raises the CONNECT password cap to 2048. Without
a secret the 256-byte cap is unchanged.

## NKEY

`cmq_nkey_verify` is raw Ed25519 (32-byte pub, 64-byte
sig). Seed / base32 nkey codec and CONNECT wiring stay
deferred.

## Performance

No secret: one pointer check on CONNECT. HMAC runs only
on the worker CONNECT path.

## Tests

`tests/test_jwt.c`

## See also

- `docs/reviews/v0.5.62.enumeration.md`
