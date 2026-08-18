# ADR 0012 — Persistent Subscription Recovery (P3)

**Status.** Accepted (v0.5.1).
**Date.** 2026-08-18.
**Context.** v0.5.0 `cmq_server_create` opened the F18 persistence
file (cmq_sublist_persist_open) and recorded SUB/UNSUB events as they
happened, but never called `cmq_sublist_persist_load` on startup.
Subscriptions were therefore lost on every restart — a real
durability hole.

## Decision

Call `cmq_sublist_persist_load` from `cmq_server_create` after the
F5 publish-replay loop, with a recovery callback that re-inserts
each SUB record into `srv->sublist` as a "ghost" `cmq_sub_ref_t`
with `client = NULL`. The ref is owned by the sublist (freed in
`cmq_sublist_free_data`).

UNSUB records on restart are no-ops (no live sub to remove).

## Rationale

The bundle flagged this as P3 with effort M. A full replay
implementation would have to recreate per-client state (account
epochs, sub-id counters, queue-group rotation), which is invasive
and unverified.

The ghost-ref model is intentionally simplified:
- **What works**: matching publish against the recovered pattern
  finds the ghost ref. The deliver path sees `client == NULL` and
  drops the message — same as if the pattern matched no subscriber.
- **What doesn't work**: messages delivered against a ghost ref don't
  reach a real subscriber. This is acceptable because:
  - The reconnecting client can re-SUB and re-create the ref
    (with its real client pointer).
  - The recovered pattern is still useful for routing decisions
    (e.g. cluster routes that consult local sublist).
- **Cleanup**: `cmq_sublist_free_data` already walks the sublist and
  frees refs. Ghost refs are freed the same way as live refs.

## Consequences

- Restart preserves the *patterns* of subscriptions, not their
  per-client state. Clients must re-connect and re-subscribe.
- A new ghost ref is inserted on every restart; if you restart
  repeatedly, the sublist accumulates one ghost per persisted SUB.
  In steady-state the cluster ends up with N×replicas refs per
  pattern, but they all hash to the same slot (subject-keyed) so
  fan-out cost is bounded.
- This is acceptable for a first iteration; full client-state
  replay is a v0.6 item.

## Alternatives Considered

- **Full client-state recovery** (recreate cmq_client_t, sub-ids,
  queue-group rotation per record). Rejected: significant scope
  expansion, requires persisting more fields (client_id, account,
  account_epoch, qg, etc.) than the current F18 format holds.
- **Skip load entirely** (only replay publishes). Rejected: leaves
  a known gap, fails the bundle's requirement.
- **Track ghost refs by sub_id and remove on UNSUB**. Rejected: no
  meaningful consumer of UNSUB during restart; treating UNSUB as
  no-op is simpler and matches the post-recovery semantics.