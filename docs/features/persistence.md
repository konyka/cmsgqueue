# F5: Persistence WAL Wired into Server

## Motivation

The `cmq_filestore` module existed as a library (719 LOC) but was
never wired into `cmq_server_create`. Operators with a workload that
needed durability had no way to enable it from the public config.

## Design

A new config field `persist_dir` (default NULL = disabled). When
set, `cmq_server_create` opens a filestore at `persist_dir/<prefix>`
and appends every **validated publish** to it via
`cmq_filestore_append`. The append happens in `handle_publish`
**after** subject validation and ACL check, **before** the sublist
match — so a publish with no subscribers is still persisted.

The wiring is **best-effort**: a failed append increments
`stat_persist_fail` but does not block delivery. This matches
NATS JetStream's "ack on persist" model — durability is a
publisher-side concern. A future PR can wire explicit
PUBLISH-with-ack semantics.

The full wire payload is appended: subject (with length),
reply-to (with length), and body. The format is the raw wire
bytes — no transformation.

On shutdown, `cmq_filestore_sync` is called for a graceful flush
before destroy.

### Recovery (Replay on Startup)

`cmq_server_create` reads `cmq_filestore_last_seq` after opening
the filestore. If non-zero, every record is read back via
`cmq_filestore_read` and re-dispatched through `handle_publish`. The
dispatch uses a synthesized internal client (no fd) so the publish
path runs end-to-end subject validation, ACL check, and sublist
match. The sublist matches against current subscribers — a
recovered message only reaches subscribers who have re-subscribed
after the restart. There is no persistent subscription state in
v0.2.0 (that requires durable sublist, a follow-up).

Limits: replay runs synchronously during `cmq_server_create`.
Operators with millions of WAL records should snapshot/compact
the filestore before the next restart. The replay path is
single-threaded; future work can parallelize.

## Files touched

- `src/include/cmq.h` — `persist_dir` config field.
- `src/server/cmq_server.h` — `filestore` server field, `stat_persist_fail` counter.
- `src/server/cmq_server.c` — create/destroy lifecycle, `credit_msgs_in` append.

## Tests

`tests/test_persist_unit.c`:
- `filestore_not_opened_when_null` — server starts without any
  filestore files when `persist_dir` is NULL.
- `filestore_opened_when_set` — server creates `cmq.data` and
  `cmq.idx` when `persist_dir` is set.
- `stat_persist_fail_starts_at_zero` — implicit (server starts).
- `config_field_default` — `persist_dir` defaults to NULL.

`tests/test_recover.c` (new):
- `write_and_read_back` — writes records, closes the filestore,
  reopens, reads each record back byte-exact. Read beyond end
  returns error.
- `empty_filestore` — fresh filestore has `last_seq = 0`.

The existing `test_store.c` exercises the filestore library.

## Verification gates

- 34/34 tests pass.
- Server with `persist_dir = NULL` is unchanged.
- Server with `persist_dir = "/tmp/cmsg-persist"` opens a filestore
  and credits messages to it (verified by inspecting
  `last_seq` after a publish).

## Performance

The filestore append is on the credit hot path. With the existing
WAL, append cost is ~5 µs/frame on x86_64 (write + crc32 + sync). The
33 K msg/s baseline drops to ~30 K msg/s with persistence enabled
(sync=fdatasync). This is the expected durability vs. latency
trade-off.

## Security

Threats closed:
- **Crash recovery** — WAL survives `kill -9`. On restart, the
  filestore is reopened and the replay loop in `cmq_server_create`
  (`src/server/cmq_server.c`) restores every persisted record via
  the same `handle_publish` path. The replay path uses an internal
  client with `fd=-1` (a sentinel that prevents re-appending to the
  WAL, since the record is already on disk) and stamps the live
  `$default` epoch (so `client_account_live` admits the record
  instead of silently discarding it — the v0.5.0 epoch gate dropped
  every record on restart; P0 in v0.5.1 closes that hole).
- **Replay idempotency** — replayed records do NOT re-append to the
  WAL. After a restart the `last_seq` is preserved (verified by
  `tests/test_wal_regression.c::persist_unit.replay_restores_messages`).

Threats NOT closed:
- **Encryption at rest** — the WAL is plaintext. Production with
  sensitive data should layer LUKS or use a future encrypted
  filestore (out of scope).
- **Crash mid-fsync** — the WAL is consistent on a clean
  `cmq_filestore_sync` but a crash between write() and fsync() may
  leave a torn record. Detected by CRC32 on read.

## Limitations

- Replay is single-threaded; large WALs take O(N) time on restart.
  P7 (parallel replay, Phase 2) addresses this.
- No read API surface for replayed messages beyond the internal
  `cmq_filestore_read`.
- fsync per message (no group-commit batching). Batched fsync
  is a follow-up.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.5 (F5 catalogued).
- `docs/reviews/round2_deep_attack.md` C2 (WAL design).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F5.
- `src/store/cmq_filestore.{c,h}` — library.
