# JWT / NKEY / JWKS (v0.5.62–65, 0.5.74–79, 0.5.82, 0.5.90–91, D3)

CONNECT may present a compact JWT in the password field
when `jwt_issuer` is set with `jwt_hmac_secret`,
`jwt_ec_pub`, `jwt_rsa_n`/`jwt_rsa_e`, and/or `jwks_json`.

## Issue (v0.5.90 / v0.5.91)

`cmq_jwt_sign_hs256` mints compact HS256.
`cmq_jwt_sign_es256` takes a 32-byte P-256 scalar (raw R||S).
`cmq_jwt_sign_rs256` takes RSA `n`/`e`/`d` (PKCS#1 v1.5).
`iss`/`sub` reject `"`, `\`, and controls. Secret cap 128.

## Checks

- `alg` must be `HS256`, `ES256`, or `RS256` (fail closed on `none`).
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

## RS256 (v0.5.77)

`jwt_rsa_n` / `jwt_rsa_e` are base64url modulus and
exponent. `n` must decode to 256–512 bytes (2048–4096 bit).
JWS signatures are PKCS#1 v1.5 bytes. `alg` must be RS256
on this path (no HS256 / ES256 confusion).

## JWKS (v0.5.65 / v0.5.74 / v0.5.77)

`jwks_json` is a JWKS document of at most 8 keys (4 KiB).
`oct`/HS256, `EC`/`P-256`/ES256, and `RSA` (`n`+`e`) are
accepted. CONNECT tokens with `kid` select that key.
Unknown `kid` fails. Missing `kid` may use
`jwt_hmac_secret`, `jwt_ec_pub`, or `jwt_rsa_n`/`jwt_rsa_e`.

`jwks_url` (v0.5.76 / v0.5.79) GETs an HTTP or HTTPS JWKS
document at server create (`/.well-known/jwks.json` if the
path is omitted). Default HTTPS port is 443. `jwks_ca` is
an optional PEM; otherwise the system CA store is used.
Mutually exclusive with `jwks_json`.

`jwks_refresh_sec` (v0.5.82) re-GETs that URL on a sidecar.
`0` is one GET at create. Config allows 5–86400 seconds.
v0.5.131: reload applies a non-zero interval to a live
sidecar. Omitted / 0 keeps the current interval.
v0.5.137: reload applies a non-empty `jwks_url` to the
live sidecar (host/path/port/tls). Omitted / empty keeps
the current URL. A bad URL fails closed. CA is preserved.
Does not re-GET; the next interval uses the new URL.
v0.5.141: reload starts the sidecar when create had a
JWKS cache but no refresher. Omitted / 0 / empty URL
keeps off. Does not GET. The first create-time GET stays
create-time.
v0.5.134: reload applies a non-empty `jwks_ca` to the live
sidecar URL. Omitted / empty keeps the current path.
`..` fails closed. The next refresh uses the new CA.
A failed GET keeps the previous ping-pong slot. CONNECT
copies key bytes out of the live slot so a later refresh
cannot overwrite material mid-verify.

## NKEY on CONNECT (v0.5.63)

When `nkey_pub` is set and JWT is not, CONNECT username is
required and the password is 128 hex chars: Ed25519
signature of `CMQNK1|<username>`.

`nkey_pub` is 64 hex chars **or** a NATS `U…` user public
(v0.5.75). `cmq_nkey_seed_decode` accepts `SU…` seeds
(derive pub only; the server does not store seeds).

`cmq_nkey_verify` remains raw Ed25519 (32-byte pub, 64-byte
sig). JWT wins if both JWT and nkey are configured.

## Reload (v0.5.119)

SIGHUP / `cmq_server_reload` copies non-empty
`auth_username`, `auth_password`, `jwt_issuer`,
`jwt_hmac_secret`, `nkey_pub`, `jwt_ec_pub`, and
`jwt_rsa_n` / `jwt_rsa_e` onto the live config.
`jwt_leeway_sec` 1–3600 replaces the stored leeway.
Empty or omitted keys keep the current values.

SIGHUP also parses a non-empty `jwks_json` into the live
JWKS cache (v0.5.120). Bad JSON keeps the previous slot.
A non-empty `jwks_url` updates the live sidecar (v0.5.137).
Reload starts a refresher when create had a cache but no
sidecar (v0.5.141). The first GET stays create-time.

## Performance

No secret / JWKS / EC / RSA / `nkey_pub`: one pointer check on CONNECT.
HMAC / ECDSA / RSA / Ed25519 run only on the worker CONNECT path.
JWKS lookup is ≤8 compares on the live cache slot. Refresh I/O
never runs on offer/PUBLISH.

## Tests

`tests/test_jwt.c`, `tests/test_nkey_auth.c`, `tests/test_jwks.c`,
`tests/test_es256.c`, `tests/test_nkeyb32.c`, `tests/test_jwksf.c`,
`tests/test_rs256.c`, `tests/test_jwkss.c`, `tests/test_jwksr.c`,
`tests/test_jwti.c`, `tests/test_jwts.c`,
`tests/test_jrf.c`, `tests/test_jca.c`, `tests/test_jru.c`,
`tests/test_jra.c`

## See also

- `docs/reviews/v0.5.62.enumeration.md`
- `docs/reviews/v0.5.63.enumeration.md`
- `docs/reviews/v0.5.65.enumeration.md`
- `docs/reviews/v0.5.74.enumeration.md`
- `docs/reviews/v0.5.75.enumeration.md`
- `docs/reviews/v0.5.76.enumeration.md`
- `docs/reviews/v0.5.77.enumeration.md`
- `docs/reviews/v0.5.79.enumeration.md`
- `docs/reviews/v0.5.82.enumeration.md`
- `docs/reviews/v0.5.90.enumeration.md`
- `docs/reviews/v0.5.91.enumeration.md`
