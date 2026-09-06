# Idempotent publish (v0.5.55, D5 phase 1)

A publisher may prefix headers with a 16-byte token so retries
do not fan out or persist twice.

```
CMQI | pid u32be | seq u64be
```

`pid` must be non-zero. Encode/parse live in `cmq_idempo_*`.

## Window

Each pid keeps the last 64 sequence numbers. A seen seq is
dropped (no ERROR, no WAL, no match). A seq older than the
window is also treated as a duplicate. Out-of-order unseen
seqs inside the window are accepted once.

256 pids. A new pid when the table is full is rejected
(`"idempo full"`).

## Performance

PUBLISH without `CMQ_FLAG_HEADERS`: unchanged. Headers
without the `CMQI` magic: one 4-byte compare.

## Coordinator

D5 phase 2 (`cmq_txn`, v0.5.60) buffers `CMQT` adds and
commits them after a durable log record.

## Tests

`tests/test_idempo.c`

## See also

- `docs/reviews/v0.5.55.enumeration.md`
