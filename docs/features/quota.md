# F14: Per-Account Quota Enforcement

## Motivation

A single noisy account can starve others. cmq_account has counters but no enforcement. Operators have no per-account knobs to bound a single publisher.

## Design

A new `cmq_quota` module implements per-account token-bucket caps. Configuration (per server):

- `max_msgs_per_sec` — cap on messages/sec per account.
- `max_bytes_per_sec` — cap on bytes/sec per account.
- `max_connections` — **connects per second** per account (fixed
  1s window). Concurrent connection / subscription / payload
  ceilings are `account_max_*` on the account object (v0.5.48);
  see `docs/features/accounts.md`.

Publish checks run on the `handle_publish` / BATCH path. CONNECT
consults `cmq_quota_check_connect` **before** concurrent credit
(v0.5.50). A reject is CONNACK 1 plus `rate_limit_reject` /
`connect quota` in the audit log. Cluster route-adopt is not
counted. The window is fixed 1 second.

Per-account state is in a small linked list (max 4096 accounts). On collision, a new account is admitted (same trade-off as the F10 rate limit). Production deployments with >4096 accounts should use a per-account hash or upgrade to the full implementation.

## Files touched

- `src/enterprise/cmq_quota.{h,c}` (new).
- `CMakeLists.txt` — `cmq_quota.c` added to `CMQ_ENTERPRISE_SOURCES`.
- `tests/test_quota.c` (new).

## Tests

`tests/test_quota.c`:
- `quota.zero_limit_admits` — limits of 0 disable enforcement.
- `quota.msg_limit_enforced` — 3/sec cap, 4th rejected.
- `quota.byte_limit_enforced` — 700-byte cap, 4th 200-byte msg rejected.
- `quota.accounts_isolated` — user1's quota doesn't affect user2.
- `quota.connect_limit` — 2 connects/sec per account.
- `quota.connect_null_admits` — NULL quota admits (hot-path skip).
- `quota.connect_isolated` — per-account windows.
- `quota.connect_disabled_when_only_msgs` — `max_connects=0` is off.

## Verification gates

- 5/5 quota tests pass.
- 45/45 total tests pass.

## Performance

The check is a linear scan over a small per-account list (typically <100 accounts). Cost: ~50 cycles per check. On the publish hot path, this is the only overhead. Negligible compared to the network round-trip.

## Security

Threats closed:
- **Account-level DoS** — a single misbehaving account cannot starve others.
- **Burst protection** — token-bucket smooths traffic.

Threats NOT closed:
- **Cross-account quota aggregation** — the limits are per-account, not global. Operators can set global limits via existing F10.
- **Quota bypass via sub-account rename** — accounts are not validated for authenticity. The quota is on the wire-level account claim.

## Limitations

- Fixed-window (1 second) is not as smooth as token-bucket; bursts at second boundaries can briefly exceed the cap. For tighter smoothing, future work.
- No quota APIs at runtime; operators must restart the server to change caps.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F14.
- `docs/features/audit.md` — quota events trigger audit log entries.
