# JWT / NKEY (v0.5.62–63, D3 phases 1–2)

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

## NKEY on CONNECT (v0.5.63)

When `nkey_pub` is set (64 hex chars) and JWT is not,
CONNECT username is required and the password is 128 hex
chars: Ed25519 signature of `CMQNK1|<username>`.

`cmq_nkey_verify` remains raw Ed25519 (32-byte pub, 64-byte
sig). JWT wins if both JWT and nkey are configured.

Seed / base32 nkey codec, JWKS, and ES256 stay deferred.

## Performance

No secret and no `nkey_pub`: one pointer check on CONNECT.
HMAC / Ed25519 run only on the worker CONNECT path.

## Tests

`tests/test_jwt.c`, `tests/test_nkey_auth.c`

## See also

- `docs/reviews/v0.5.62.enumeration.md`
- `docs/reviews/v0.5.63.enumeration.md`
