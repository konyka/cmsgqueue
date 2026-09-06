# F15: REQUEST/RESPONSE Inbox HoL Fix

## Motivation

The REQUEST/RESPONSE pattern in CMSGQueue shares the publish
budget with regular PUBLISH frames. A slow responder holds the
head-of-line lock, blocking other publishes on the same connection.
NATS Server isolates REQUESTs into a separate "inbox" budget; CMQ
did not.

## Design

A new config field `inbox_max_pending` (default 0 = disabled).
SIGHUP copies a non-zero value onto the live config (v0.5.122).
When set, the server tracks per-connection pending REQUESTs
(`cmq_client_t::inbox_pending`). On `handle_request`:

```c
if (inbox_max_pending > 0 && c->inbox_pending >= inbox_max_pending) {
    cmq_send_error(c, "inbox full");
    return;
}
c->inbox_pending++;
```

On `handle_response`:
```c
if (c->inbox_pending > 0) c->inbox_pending--;
```

The simple counter is racy in the case of out-of-order RESPONSEs
(both in the wild and in the implementation), but the worst case
is over-counting — which favors availability. A RESPONSE without
a matching REQUEST is rare and just leaves a non-zero counter
that drains on the next disconnect.

## Files touched

- `src/include/cmq.h` — `inbox_max_pending` config field.
- `src/server/cmq_config.c` — config parse + validation.
- `src/server/cmq_server.h` — `inbox_pending` per-conn counter.
- `src/server/cmq_server.c` — `handle_request` and `handle_response`.
- `tests/test_inbox.c` — 2 tests.

## Tests

`tests/test_inbox.c`:
- `inbox.config_field_set` — verifies the config field is settable.
- `inbox.default_zero_disables` — verifies default is 0 (disabled).

## Verification gates

- 34/34 tests pass (was 33, +1 test_inbox with 2 tests).

## Performance

The check is one integer comparison on the REQUEST hot path. Cost:
~1 cycle. No measurable impact on the publish baseline.

## Security

Threats closed:
- **Slow responder DoS** — bounded by `inbox_max_pending` per
  connection. An attacker cannot flood the server with pending
  REQUESTs.

Threats NOT closed:
- Cross-connection DoS: a single attacker can open many
  connections. Mitigated by `max_clients` (F10, already in
  place) and `max_connects_per_sec` (F10).

## Limitations

- Counter is racy on out-of-order RESPONSEs. Production with strict
  per-REQUEST tracking needs an `inbox_id` field. Deferred.
- The counter does NOT track REQUESTs that have been routed to a
  cluster peer (those are tracked at the cluster layer). The
  local-conn counter is sufficient for the local HoL protection.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.15 (F15).
- `docs/reviews/round2_deep_attack.md` A3 (inbox HoL critique).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F15.
