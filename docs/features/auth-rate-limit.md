# F8: Auth Brute-Force Rate Limit

## Motivation

scrypt verify (F8) is ~100ms per call. An attacker can mount an online brute-force attack at ~10 attempts/sec without rate limiting — at 100K password attempts in 3 hours, even moderate-strength passwords are at risk. NATS Server doesn't have this; CMSGQueue adds it.

## Design

A per-IP rate limit on failed `CONNECT` attempts. After 10 attempts within a 1-second window, subsequent attempts from the same IP are rejected with `cmq_send_connack(c, 4)` ("rate limit") without invoking the password verify. The check uses `getpeername` on the connected socket, keyed on the source IP.

The rate lock (F10) is reused. The auth path is mutual-exclusion with the connect path but distinct from the publish hot path.

## Files touched

- `src/server/cmq_server.c` — `CMQ_OP_CONNECT` handler includes the rate check.
- `src/server/cmq_server.{h}` — already has the F10 rate_lock.
- `tests/test_auth_ratelimit.c` (new).

## Tests

`tests/test_auth_ratelimit.c`:
- `auth_rl.under_limit_admits` — 10 attempts admitted.
- `auth_rl.over_limit_rejects` — 11th attempt rejected.
- `auth_rl.different_ips_isolated` — IP isolation.
- `auth_rl.window_rolls_over` — 1s window reset.

## Verification gates

- 4/4 auth-ratelimit tests pass.
- 45/45 total tests pass.

## Performance

The check is a linear scan over a small slots table (1024 entries). Cost: ~50 cycles per CONNECT. Negligible compared to the scrypt verify (~100ms).

## Security

Threats closed:
- **Online brute-force** — capped at 10 attempts/sec/IP, regardless of scrypt's cost.
- **Cost amplification** — even if scrypt verify were free, the cap holds.

Threats NOT closed:
- **Distributed brute-force** — each IP gets its own bucket. A botnet can still attack.
- **Slow-rotation** — if a legitimate user hits the cap, they wait 1 second.

## Limitations

- 1024-slot fixed table. New IPs after table full are admitted (same as F10).
- 10 attempts/sec is hardcoded. Future work: configurable via `cmq_config_t`.
- Cap is uniform across all IPs. Premium accounts could get higher caps (future work).

## See also

- `docs/reviews/hyperplan-v030-plan.md` F8b.
- `docs/features/password-hash.md` — F8 (the verify path that this protects).
- `docs/features/rate-limit.md` — F10 (the underlying rate-limiter).
- `docs/features/audit.md` — F13 (rate-limit rejections are audited).
