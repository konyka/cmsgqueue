# ADR 0012 - Persistent Subscription Recovery (P3)

**Status.** Accepted (v0.5.1), superseded in part by the shipped final-state
replay implementation.

**Date.** 2026-08-18.

**Context.** v0.5.0 `cmq_server_create` opened the F18 persistence file
(`cmq_sublist_persist_open`) and recorded SUB/UNSUB events as they happened,
but never called `cmq_sublist_persist_load` on startup. Subscriptions were
therefore lost on every restart. This ADR records the historical recovery
decision and the later implementation details that supersede its old
unsubscribe and accumulation assumptions.

## Decision

Call `cmq_sublist_persist_load` from `cmq_server_create` after the F5
publish-replay loop. The callback applies records sequentially and tracks
recovered refs by `sub_id`: a SUB creates or replaces a ghost
`cmq_sub_ref_t` with `client = NULL`, and an UNSUB removes the matching ghost
when present. The resulting sublist is the final WAL state. The refs are
owned by the sublist and freed during sublist cleanup.

Malformed WAL records, invalid subjects, and callback failures fail startup
with `CMQ_ERR_INVALID_ARG`; the server is destroyed before it accepts clients.

## Rationale

The bundle flagged this as P3 with effort M. Recovery restores server-side
subject patterns, not per-client state. It does not recreate TCP clients,
account epochs, sub-id counters, or queue-group rotation.

The ghost-ref model remains intentionally limited:

- **What works:** the final recovered pattern participates in sublist
  matching, and a reconnecting client can create a live ref by subscribing
  again.
- **What does not work:** a ghost has no client, so recovery does not restore
  the crashed connection or its per-client delivery state.
- **Cleanup:** `cmq_sublist_free_data` frees ghost refs in the same way as
  live refs.
- **Scope limitation:** WAL `sub_id` values are not client-scoped. Recovery
  cannot distinguish the same ID used by different clients.

## Consequences

- Restart preserves the final subject patterns, not per-client state. Clients
  must reconnect and resubscribe for live client refs.
- Replaying UNSUB records removes earlier recovered refs, so the sublist does
  not accumulate one ghost for every historical SUB record.
- Recovery is O(N_records), including superseded records.

## Alternatives Considered

- **Full client-state recovery** (recreate `cmq_client_t`, sub-ids, queue-group
  rotation per record). Rejected: significant scope expansion, and the current
  F18 format does not persist enough client state.
- **Skip load entirely** (only replay publishes). Rejected: leaves a known
  durability gap.
- **Track ghost refs by `sub_id` and remove on UNSUB**. Adopted in the shipped
  implementation so replay produces final WAL state. This ID is still not
  client-scoped.
