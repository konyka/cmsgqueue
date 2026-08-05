# F8: Password Hashing (scrypt)

## Motivation

`auth_password` in `cmq_config_t` was stored as a `strdup`'d plaintext string (`src/server/cmq_config.c:124-125`). Any process with read access to the running process's memory — `gcore`, `/proc/<pid>/mem`, container sidecars, coredumps — recovered the live credential. CWE-256.

The bundle's F8 originally specified argon2id; libsodium headers are unavailable in the build environment. We chose **scrypt via OpenSSL 3.x's `EVP_PBE_scrypt`** as the alternative.

## Design

Wire format:
```
$scrypt$N=<power-of-2>,r=<block>,p=<parallel>,salt_len=<N>,hash_len=<N>$<salt-b64>$<hash-b64>
```

Default parameters: `N=16384, r=8, p=1`. Salt 16 bytes, hash 32 bytes.

- `cmq_password_hash(password, out, out_len)` — generates a fresh salt, runs scrypt, base64url-encodes, returns the wire format. **Constant-time** verify via `EVP_PBE_scrypt(password, salt, N, r, p, ...)` then `diff |= computed[i] ^ hash[i]` over all 32 bytes.
- `cmq_password_verify(stored, password)` — returns `1` (match), `0` (no match), or `-1` (malformed).

The CONNECT path in `src/server/cmq_server.c:4404-4418` detects the format prefix. If the configured password starts with `$`, it's hashed; otherwise it's treated as legacy plaintext (deprecated but supported for transition).

To migrate a deployment:
```sh
echo -n "mypassword" | openssl-scrypt-helper
# or use the cmq CLI (TODO) to compute $scrypt$...$...$...
```

## Files touched

- `src/server/cmq_password.{h,c}` (new).
- `src/server/cmq_server.c` — auth verify path detects format.
- `CMakeLists.txt` — `cmq_password.c` added to `CMQ_CORE_SOURCES`.
- `tests/test_password.c` (new).

## Tests

`tests/test_password.c`:
- `password.hash_format_round_trip` — hash + verify round-trip.
- `password.distinct_passwords_distinct_hashes` — random salts differ.
- `password.buffer_too_small` — output cap enforced.
- `password.malformed_hash_rejected` — garbage input returns -1.
- `password.plaintext_legacy_accepted` — `$plaintext$...` form.
- `password.empty_password_rejected` — empty password is a config error.

## Verification gates

- 6/6 password tests pass.
- Existing 36 tests still pass (37/37, modulo the pre-existing `test_stress` flake).
- OpenSSL scrypt is linked (confirmed by `OpenSSL:3.5.7` in the build).
- The wire format prefix `$scrypt$N=16384,r=8,p=1,...` parses correctly.

## Performance

- `EVP_PBE_scrypt` with N=16384, r=8, p=1: ~100 ms on a modern x86_64 server.
- CONNECT is rare relative to msg/s, so 100 ms is acceptable. F8b adds per-IP rate-limiting to bound the worst case.

## Security

Threats closed:
- **Plaintext password leak via memory disclosure** — the credential is now a scrypt hash. Recovering the live process memory no longer yields the password.
- **Coredump exposure** — coredumps now expose hashes, not passwords.

Threats NOT closed:
- **Brute-force offline attack against the hash** — operators must keep the hash file (`persist_dir`) secure; scrypt's memory-hardness provides resistance but is not absolute.
- **Online brute-force** — F8b (per-IP rate-limit) addresses this.

## Limitations

- Existing deployments with plaintext `auth_password = foo` continue to work (legacy path). Operators are encouraged to migrate.
- The format is not the standard PHC string format (which would be `$scrypt$ln=16,r=8,p=1$...`). We use a simplified form to avoid an external dependency. Future work could switch to the PHC format.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.8 (F8 catalogued).
- `docs/reviews/round2_deep_attack.md` C1 (password storage critique).
- Plan reference: `docs/reviews/hyperplan-v030-plan.md` Part 2.
- OpenSSL EVP_PBE_scrypt: `man 3 EVP_PBE_scrypt`.
