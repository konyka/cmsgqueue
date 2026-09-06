# JWT / NKEY / JWKS (v0.5.62–65, D3 phases 1–3)

CONNECT may present a compact HS256 JWT in the password
field when `jwt_issuer` is set with `jwt_hmac_secret` and/or
`jwks_json`. Verify uses OpenSSL HMAC. Tokens are never
issued here.

## Checks

- `alg` must be `HS256` (fail closed on `none`).
- `iss` must equal `jwt_issuer`.
- `exp` required; reject if `now > exp + leeway`.
- `nbf` optional; reject if `now + leeway < nbf`.
- Default leeway is 60 seconds (`jwt_leeway_sec`).
- `sub`, when present, becomes the account name.

JWT mode raises the CONNECT password cap to 2048. Without
a secret the 256-byte cap is unchanged.

## JWKS (v0.5.65)

`jwks_json` is a JWKS document of at most 8 `oct` keys
(4 KiB). CONNECT tokens with `kid` select that key.
Unknown `kid` fails. Missing `kid` may use
`jwt_hmac_secret` when that is set.

HTTP JWKS fetch, RSA/EC, and ES256 stay deferred.

## NKEY on CONNECT (v0.5.63)

When `nkey_pub` is set (64 hex chars) and JWT is not,
CONNECT username is required and the password is 128 hex
chars: Ed25519 signature of `CMQNK1|<username>`.

`cmq_nkey_verify` remains raw Ed25519 (32-byte pub, 64-byte
sig). JWT wins if both JWT and nkey are configured.

Seed / base32 nkey codec stays deferred.

## Performance

No secret / JWKS / `nkey_pub`: one pointer check on CONNECT.
HMAC / Ed25519 run only on the worker CONNECT path.
JWKS is parsed once at create; lookup is ≤8 compares.

## Tests

`tests/test_jwt.c`, `tests/test_nkey_auth.c`, `tests/test_jwks.c`

## See also

- `docs/reviews/v0.5.62.enumeration.md`
- `docs/reviews/v0.5.63.enumeration.md`
- `docs/reviews/v0.5.65.enumeration.md`
