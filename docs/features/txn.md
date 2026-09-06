# Transaction coordinator (v0.5.60, D5 phase 2)

A publisher may prefix headers with a 13-byte token so several
PUBLISH frames commit or abort together.

```
CMQT | txn_id u64be | op u8
```

`txn_id` must be non-zero. Ops: `BEGIN=1` `ADD=2` `COMMIT=3`
`ABORT=4`. Encode/parse live in `cmq_txn_*`.

## Visibility

`ADD` is buffered (no WAL, no fanout). `COMMIT` fsyncs a
record (when a log dir is set) then applies each add via
`cmq_server_publish`. `ABORT` drops the buffer. A second
`COMMIT` of a known id is a no-op.

## Persist

`cmq_txn_set_log(dir)` uses `{dir}/cmq.txn`. The server
enables this when `persist_dir` is set. Crash before the
COMMIT record = not committed. Recover loads ids only
(no re-fanout).

## Performance

PUBLISH without `CMQ_FLAG_HEADERS`: unchanged. Headers
without `CMQT`: one 4-byte compare (after the CMQI check).

32 in-flight txns, 8 ops, 1024-byte payloads.

## Tests

`tests/test_txn.c`

## See also

- `docs/reviews/v0.5.60.enumeration.md`
- `docs/features/idempo.md`
