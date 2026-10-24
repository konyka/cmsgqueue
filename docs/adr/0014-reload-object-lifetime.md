# ADR 0014 — Reload Object Lifetime (P1 reload UAF)

**Status.** Accepted (v0.5.1).
**Date.** 2026-08-18.
**Context.** v0.5.0 `cmq_server_reload` swapped `srv->acl` and
`srv->blocklist` while worker threads read them lock-free
(`cmq_server.c:2954`, `:4437`, `:7174-7205`). The race window allowed
a worker to dereference a freed pointer — classic use-after-free.

The race is not deterministically unit-testable in the single-threaded
cmq_test.h harness (per the v0.5.1 adversarial review, B6). The bundle
mandated a structural fix: **make the race impossible by ownership of
the pointer**.

## Decision

Introduce `cmq_rch_t` — a refcounted handle that owns a typed pointer
plus an atomic refcount. Reader API:

```c
void *p = cmq_rch_acquire(handle);  // atomic increment + load
use(p);
cmq_rch_release(handle, p);         // atomic decrement; free if last
```

Reload API:

```c
cmq_rch_t *nh = cmq_rch_new(new_obj, free_fn);
cmq_rch_t *old = cmq_rch_swap(&srv->acl_h, nh);   // atomic exchange
if (old) cmq_rch_release_owner(old);  // drops the owner ref; frees if last
```

`cmq_rch_swap` is an unconditional atomic exchange: it stores `nh` in the
slot and returns the previous handle. `cmq_rch_release_owner` drops the
slot's own reference and runs `free_fn` on the object when the last
in-flight reader lets go. (`cmq_rch_release(h, NULL)` is for readers only
and never runs `free_fn`, so it must not be used to retire the old handle.)

`cmq_server_t` now exposes `acl_h` and `blocklist_h` instead of raw
`acl` / `blocklist` pointers. `handle == NULL` means the feature is
disabled (same NULL semantics as before).

## Rationale

Refcounting has well-understood semantics and one-atomic-op hot-path
cost (CAS on increment; the swap is a single atomic exchange, not a lock).

The alternative — RCU quiescent-state — would require a per-client
quiescent flag or read-side critical section tracking, neither of which
exists in the server today. RCU is the next logical step if rch becomes
a hot-path bottleneck, but the bundle explicitly noted this isn't a
unit-testable scenario, so the simpler fix lands first.

## Consequences

- Hot path: one atomic CAS (`acquire`) + one atomic fetch-sub
  (`release`) per publish (for ACL) and per accept (for blocklist).
  Approximately 20-40 ns each on x86.
- The handle is malloc'd and freed; in steady state the count is 1
  (no extra alloc churn). At reload, both old and new coexist briefly
  (refcount=2 max).
- `cmq_acl_free` / `cmq_blocklist_free` retain ownership of the data;
  the handle owns the lifetime.
- All reader sites (`handle_publish` ACL check, `accept_cb`
  blocklist check) updated to acquire/release.

## Fix history

- `cmq_rch_swap` was introduced (v0.5.1) as a compare-exchange whose
  expected value was the new handle, so it never matched, never stored
  the new handle, and returned the *current* handle. The reload callers
  then released the owner reference of the handle still installed in the
  slot and leaked the freshly built one. It is now `atomic_exchange`, and
  `tests/test_rch.c` / `tests/test_rch_overflow.c` assert that the slot
  holds the new handle and the old one is returned.

## Alternatives Considered

- **Mutex around reload** (sequentially consistent block). Rejected:
  reload is rare but blocks the entire worker thread for tens of
  milliseconds. Head-of-line blocking on hot path.
- **RCU with quiescent flags**. Deferred — see rationale.
- **CoW pointers with atomic snapshot** (read snapshot, write pointer,
  readers take snapshot under load-acquire, writer frees after
  poll-quiescent). Rejected as over-engineered for this case.