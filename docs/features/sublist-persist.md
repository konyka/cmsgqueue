# F18: Persistent Subscription State (STUB)

## Status

**STUB.** The full persistent sublist requires refactoring the in-memory `cmq_sublist` to also write to a WAL stream. The current implementation has no WAL integration.

The library API (`cmq_sublist_persist.{h,c}`) is in place; the implementation is deferred. See `docs/reviews/hyperplan-v030-plan.md` for the full design.

## Design (deferred)

The flow is:

1. `SUBSCRIBE`: `cmq_sublist_add` writes the subscription to a dedicated WAL stream (separate from the publish WAL in F5).
2. `UNSUBSCRIBE`: `cmq_sublist_remove` deletes from the WAL.
3. Startup: `cmq_sublist_persist_load` reads all entries from the WAL into the in-memory sublist, restoring the state.
4. Recovery: the F5 replay loop dispatches persisted publishes to currently-subscribed subjects only. Subscriptions in the WAL at the time of the crash are restored; clients that were connected at the time of the crash are NOT restored (TCP connections cannot be replayed).

## Files touched (stub only)

- `src/server/cmq_sublist_persist.{h,c}` (new) — API contract.
- `tests/test_sublist_persist_stub.c` (new) — verifies stub returns -1.

## Tests

`tests/test_sublist_persist_stub.c`:
- `sublist_persist.open_returns_null_until_implemented` — stub returns NULL.
- `sublist_persist.record_returns_error` — record APIs return -1.

## Verification gates

- 2/2 stub tests pass.
- 45/45 total tests pass.

## Performance

(Implementation pending.) Expected: O(N_subs) recovery on startup where N_subs is the number of subscriptions. For 100K subscriptions, ~10 ms recovery on warm storage.

## Security

(Implementation pending.) The WAL is plaintext (encryption at rest is out of scope for F18).

## Limitations

- Full implementation deferred.
- No replay of TCP connections (impossible).
- No "imports" between subscription streams.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F18.
- `docs/features/persistence.md` — F5 publish WAL (separate stream).
- `docs/reviews/round3_deep_gap_analysis.md` — the gap analysis that motivated F18.
