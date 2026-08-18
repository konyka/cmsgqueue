# ADR 0016 — Async WAL Ring (P2 partial — cached EOF offsets)

**Status.** Partially accepted (v0.5.1). Full async ring deferred.
**Date.** 2026-08-18.
**Context.** v0.5.0 `cmq_filestore_append` performed 4 syscalls
(`seek_end`+`ftello` × 2) on every publish to compute the next
write offset. This dominated the WAL hot path (~250-500 µs per
publish).

## Decision

Replace the per-append `seek_end`+`ftello` dance with **cached EOF
offsets** maintained in the `cmq_filestore_t` struct. After each
successful write, advance `data_end_off` by `HDR_SIZE + payload_len`
and `idx_end_off` by 8. The first append after open (or after a
`filestore_read`) reseeds the cache from one `seek_end`+`ftello`.

## Rationale

This is the lowest-risk subset of the full async ring (SPSC + worker
thread + backpressure) the bundle planned. The full ring was
estimated at 260-380 LOC + a new failure mode (ring-full backpressure,
shutdown drain) — too much surface area for v0.5.1.

The cached-offset optimization captures the dominant cost (4 syscalls
eliminated per append) without introducing a new thread, ring buffer,
or backpressure policy. Risk is bounded: a stale cache only happens if
a reader moves the file position mid-append sequence, and the read
path explicitly invalidates the cache.

## Consequences

- WAL append throughput: ~360K appends/sec on a single thread (measured
  in `tests/test_wal_throughput.c`).
- Default ctest stays green (62/62).
- Bench publish-path gate still passes (msg_per_sec=33517).
- Read-after-write still works; cache is invalidated on read.
- The full async ring (SPSC + worker + sync_interval) is a v0.6 item
  per the bundle's Phase 2 plan.

## Alternatives Considered

- **True async ring (SPSC + worker)**. Deferred — see above.
- **`O_APPEND` open flag**. Rejected: stdio's `fopen("a+b")` already
  uses O_APPEND on POSIX; the cost was stdio's `seek_end+ftello` for
  offset computation, not the write syscall itself. So this is the
  fix.
- **Switch to `pwrite`/`pread` (POSIX 1:1) and maintain offsets
  in memory**. Equivalent to the cached-offset fix; the stdio path
  is left in place for the rollback-on-failure paths that depend on
  `ftruncate` semantics.