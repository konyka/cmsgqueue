# Transaction coordinator (v0.5.60 / v0.5.80, D5 phases 2–3)

A publisher may prefix headers with a 13-byte token so several
PUBLISH frames commit or abort together.

```
CMQT | txn_id u64be | op u8
```

`txn_id` must be non-zero. Ops: `BEGIN=1` `ADD=2` `COMMIT=3`
`ABORT=4` `PREPARE=5`. `VOTE=6` is 30 bytes:
`CMQT | id | VOTE | yes u8 | node[16]`. Encode/parse live
in `cmq_txn_*`.

## Visibility

`ADD` is buffered (no WAL, no fanout). `COMMIT` fsyncs a
record (when a log dir is set) then applies each add via
`cmq_server_publish`. `ABORT` drops the buffer. A second
`COMMIT` of a known id is a no-op.

## 2PC (v0.5.80)

No live routes: COMMIT is unchanged. With live routes the
coordinator registers each `remote_id`, prepares, fans out
BEGIN/ADD/PREPARE, waits ≤200 ms for VOTEs, then COMMIT or
ABORT. A route peer prepares locally and VOTEs with
`cluster_node_id` (no re-fanout). Missing votes are NO.
At most 8 participants.

## Persist

`cmq_txn_set_log(dir)` uses `{dir}/cmq.txn`. The server
enables this when `persist_dir` is set. Crash before the
COMMIT record = not committed. Recover loads ids only
(no re-fanout).

## Performance

PUBLISH without `CMQ_FLAG_HEADERS`: unchanged. Headers
without `CMQT`: one 4-byte compare (after the CMQI check).
No live routes: COMMIT does not wait.

32 in-flight txns, 8 ops, 1024-byte payloads.

## Tests

`tests/test_txn.c`, `tests/test_txn2.c`

## See also

- `docs/reviews/v0.5.60.enumeration.md`
- `docs/reviews/v0.5.80.enumeration.md`
- `docs/features/idempo.md`
