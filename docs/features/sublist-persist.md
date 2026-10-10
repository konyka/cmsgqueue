# F18: Persistent Subscription State

## Status

**Shipped.** `SUBSCRIBE` and `UNSUBSCRIBE` record state in the subscription
WAL. On startup, the server replays that WAL sequentially before accepting
clients and keeps the final state for each `sub_id`. Restored entries are
server-side ghost subject references, not TCP clients or connection state.
Publish replay runs before subscription WAL replay, so restored subscriptions
do not receive historical publishes from the same restart.

The library API (`cmq_sublist_persist.{h,c}`) is wired into the server when
`persist_dir` is configured. A malformed WAL record, an invalid subject, or a
recovery callback failure rejects startup. The server cleans up and returns
`CMQ_ERR_INVALID_ARG` rather than accepting clients with partial subscription
state.

## Design

The flow is:

1. `SUBSCRIBE`: `cmq_sublist_add` writes the subscription to a dedicated WAL
   stream, separate from the publish WAL in F5.
2. `UNSUBSCRIBE`: `cmq_sublist_remove` writes an unsubscribe record to the WAL.
3. Startup: `cmq_sublist_persist_load` reads records in file order. A
   `SUBSCRIBE` creates or replaces the ghost ref for its `sub_id`; an
   `UNSUBSCRIBE` removes that ref when present. The resulting sublist is the
   WAL's final state.
4. Recovery: the F5 publish replay loop runs first. Subscription recovery then
   restores subject patterns for routing and for new live subscriptions, but it
   cannot restore clients that were connected at the time of the crash.

## WAL format and validation

The WAL is plaintext with one record per line:

- `S <sub_id> <subject> <account>` for subscribe.
- `U <sub_id>` for unsubscribe.

Record syntax is strict. Malformed or truncated lines, malformed numeric IDs,
missing fields, and invalid subjects fail the load. The `sub_id` in the WAL is
not client-scoped, so reuse or collision across clients is not disambiguated
during recovery.

## Tests

- Startup recovery tests cover restored subscriptions and final-state
  unsubscribe replay.
- WAL parser tests cover malformed IDs and subjects, and callback failure
  propagation.

## Verification gates

The shipped implementation is covered by the repository's startup recovery,
unsubscribe replay, malformed WAL, and callback failure tests.

## Performance

Recovery is a sequential scan of the WAL. Its cost is O(N_records), where
N_records includes superseded subscriptions and unsubscribe records.

## Security

The WAL is plaintext. Encryption at rest is out of scope for F18.

## Limitations

- TCP connections and per-client connection state are not replayed.
- `sub_id` is not client-scoped in the WAL, so identical IDs from different
  clients cannot be distinguished during recovery.
- There are no imports between subscription streams.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F18.
- `docs/features/persistence.md` - F5 publish WAL, a separate stream.
- `docs/reviews/round3_deep_gap_analysis.md` - the gap analysis that motivated F18.
