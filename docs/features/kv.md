# KV last-value store (v0.5.58, D4 phase 2)

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

## Not in this increment

Object store and a partition / bucket server path.

## Tests

`tests/test_kv.c`

## See also

- `docs/reviews/v0.5.58.enumeration.md`
- `docs/features/persistence.md` (key compact)
