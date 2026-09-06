# KV last-value store (v0.5.58–67, D4 phases 2 and 4)

`cmq_kv` keeps one value per key. Default create is memory
only. `cmq_kv_set_persist(dir, prefix)` opens a filestore and
replays the live WAL with last-wins.

## API

- `put` — overwrite or insert. `-2` table full, `-3` value too large.
- `get` — copy into the caller buffer.
- `del` — drop the key. Persist writes a `CMQK` tombstone
  (empty value) so reopen does not restore it.

## Persist

Uses `cmq_filestore_key_encode` (`CMQK`). Load scans live
sequence numbers only. Do not enable `set_rotate_bytes` on a
KV prefix in this version (sealed `.1` is not replayed).

## Limits

Keys: `[A-Za-z0-9._-]`, max 256. Values: 1024 bytes.
Slots: 256 (or fewer at create).

## Bucket PUBLISH path (v0.5.67)

PUBLISH to `$KV.<bucket>.<key>` updates a named bucket
(`cmq_kvb`). Empty payload deletes. Non-`$` subjects pay
one byte compare. At most 8 buckets. Fanout still runs so
watchers see the write. Optional persist is
`{persist_dir}/kv_<bucket>` when `persist_dir` is set.

Bucket names: `[A-Za-z0-9_-]`, max 32.

REQUEST `$KV.<bucket>.<key>` (v0.5.70) returns the value to
reply-to, or an empty body on miss.

## Tests

`tests/test_kv.c`, `tests/test_kvb.c`, `tests/test_kvreq.c`

## See also

- `docs/reviews/v0.5.58.enumeration.md`
- `docs/reviews/v0.5.67.enumeration.md`
- `docs/features/persistence.md` (key compact)
