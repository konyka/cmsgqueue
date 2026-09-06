# Per-account limits (v0.5.48)

Account objects already counted connections, subscriptions, and
bytes. This version enforces NATS-style **hard caps** on those
live counters.

## Knobs

| Config key | Meaning | Default |
|---|---|---|
| `account_max_connections` | Concurrent connections per account | `0` (unlimited) |
| `account_max_subscriptions` | Concurrent subscriptions per account | `0` (unlimited) |
| `account_max_payload` | Per-message payload bytes | `0` (unlimited) |

`0` disables the individual cap. The publish path still honours
the global `max_payload_size`; the account cap cannot raise it.

These are **not** the F14 quota knobs. `max_connections_per_account`
remains a per-second connect-rate window inside `cmq_quota`.

## API

```c
cmq_account_manager_set_defaults(mgr, max_conn, max_sub, max_payload);
cmq_account_set_limits(acc, max_conn, max_sub, max_payload);
int cmq_account_check_payload(acc, bytes); /* 0 ok, -1 over */
```

Defaults copy onto **new** accounts only. Soft-delete +
`cmq_account_create` of the same name keeps the previous limits.

`cmq_account_inc_connections` / `inc_subscriptions` return:

- `0` — credit stuck
- `-1` — inactive / epoch race
- `-2` — at the concurrent cap

CONNECT treats either failure as CONNACK 1. SUBSCRIBE sends
SUBACK 1 and stays up on `-2`.

## Performance

Unlimited CONNECT/SUBSCRIBE: one atomic load + compare, then the
same `fetch_add` as before. PUBLISH: one extra integer compare
when the cached `account_max_payload` is 0.

## See also

- `docs/features/quota.md` — rate windows, not concurrent caps
- `docs/reviews/v0.5.48.enumeration.md`
