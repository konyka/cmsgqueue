# JWT / NKEY / JWKS (v0.5.62–65, 0.5.74–76, D3 phases 1–6)

CONNECT may present a compact JWT in the password field
when `jwt_issuer` is set with `jwt_hmac_secret`,
`jwt_ec_pub`, and/or `jwks_json`. Tokens are never issued
here.

## Checks

- `alg` must be `HS256` or `ES256` (fail closed on `none`).
- `iss` must equal `jwt_issuer`.
- `exp` required; reject if `now > exp + leeway`.
- `nbf` optional; reject if `now + leeway < nbf`.
- Default leeway is 60 seconds (`jwt_leeway_sec`).
- `sub`, when present, becomes the account name.

JWT mode raises the CONNECT password cap to 2048. Without
a secret the 256-byte cap is unchanged.

## ES256 (v0.5.74)

`jwt_ec_pub` is 128 hex chars of the P-256 public point
(X||Y). JWS signatures are raw R||S (64 bytes). `alg` must
be ES256 on this path (no HS256 confusion).

## JWKS (v0.5.65 / v0.5.74)

`jwks_json` is a JWKS document of at most 8 keys (4 KiB).
`oct`/HS256 and `EC`/`P-256`/ES256 are accepted. CONNECT
tokens with `kid` select that key. Unknown `kid` fails.
Missing `kid` may use `jwt_hmac_secret` or `jwt_ec_pub`.

`jwks_url` (v0.5.76) GETs an HTTP JWKS document at server
create (`/.well-known/jwks.json` if the path is omitted).
Mutually exclusive with `jwks_json`. HTTPS and refresh stay
deferred. RSA stays deferred.

## NKEY on CONNECT (v0.5.63)

When `nkey_pub` is set and JWT is not, CONNECT username is
required and the password is 128 hex chars: Ed25519
signature of `CMQNK1|<username>`.

`nkey_pub` is 64 hex chars **or** a NATS `U…` user public
(v0.5.75). `cmq_nkey_seed_decode` accepts `SU…` seeds
(derive pub only; the server does not store seeds).

`cmq_nkey_verify` remains raw Ed25519 (32-byte pub, 64-byte
sig). JWT wins if both JWT and nkey are configured.

## Performance

No secret / JWKS / EC / `nkey_pub`: one pointer check on CONNECT.
HMAC / ECDSA / Ed25519 run only on the worker CONNECT path.
JWKS is parsed once at create; lookup is ≤8 compares.

## Tests

`tests/test_jwt.c`, `tests/test_nkey_auth.c`, `tests/test_jwks.c`,
`tests/test_es256.c`, `tests/test_nkeyb32.c`, `tests/test_jwksf.c`

## See also

- `docs/reviews/v0.5.62.enumeration.md`
- `docs/reviews/v0.5.63.enumeration.md`
- `docs/reviews/v0.5.65.enumeration.md`
- `docs/reviews/v0.5.74.enumeration.md`
- `docs/reviews/v0.5.75.enumeration.md`
- `docs/reviews/v0.5.76.enumeration.md`
