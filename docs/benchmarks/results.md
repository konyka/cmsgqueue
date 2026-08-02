# CMSGQueue Benchmark Results

## Workload

`examples/benchmark -c 10 -n 10000 -t 1` (10 publishers, 1 subscriber,
10 000 64-byte messages, single worker thread). Release build
(`-DCMAKE_BUILD_TYPE=Release`), 5 runs each side.

- `after`: HEAD (`cfbae48`) — includes the eight commits shipped in the
  performance + correctness pass (parser frame-node slab, worker
  payload mpool, slab O(1) free cache, cond-var shutdown, redundant
  lock removal).
- `before`: `84ec2b9` — base prior to those five performance commits
  (docs, CI, and regression-test commits are present in both sides).

Raw per-run transcripts live next to this file as `after_1.txt` …
`after_5.txt` and `before_1.txt` … `before_5.txt`.

## Numbers

| Metric (mean of 5) | before | after | Δ |
|---|---|---|---|
| Publish time (s) | 0.0042 | 0.0044 | ~noise |
| Publish throughput (msg/s) | ~2.31 M | ~2.27 M | ~noise |
| Received / sent | 7671 / 10 000 | 7577 / 10 000 | noise |
| End-to-end throughput (msg/s) | ~34 453 | ~33 840 | −1.8 % (noise) |
| Avg latency (ms) | 0.029 | 0.030 | noise |

End-to-end spread (best ↔ worst) per side:

| Side | min | max | spread |
|---|---|---|---|
| after e2e msg/s | 33 685 | 34 137 | 452 (1.3 %) |
| before e2e msg/s | 33 524 | 35 351 | 1 827 (5.2 %) |

The after side is *slightly tighter* run-to-run — consistent with the
alloc churn being removed from the hot path — but the absolute mean sits
inside run-to-run noise.

## Reading

For the default `examples/benchmark` workload the performance commits
do **not** show a measurable end-to-end win. The reasons:

1. **Workload shape.** 10 publishers, 1 subscriber, 1 worker, 64-byte
   payloads. The parser spends a small fraction of the wall clock on
   allocation; the rest is `read()` / `write()` and one mutex hand-off.
   With glibc's per-thread caches, `malloc(64)` is essentially a
   bump-alloc, so the slab win collapses to noise.
2. **No cross-worker fanout.** The `cmq_mpool` change is invisible when
   every SEND stays on the same worker; the worker msg queue never
   fills, so the mbox path is not exercised.
3. **No big-payload traffic.** `cmq_mpool` only kicks in for payloads
   ≤ 64 KiB but the savings relative to `malloc` for tiny payloads are
   below the noise floor.
4. **No long-running shutdown.** The cond-var shutdown change is a
   worst-case latency improvement at destroy time; it does not move the
   steady-state benchmark.

The optimization work is still **correct** — see `tests/test_*` passing
under ASan/UBSan/TSan — and **qualitatively** valuable:

- removes `malloc` from the per-frame and per-SEND hot path;
- turns the slab free path from O(pages) into O(1) for steady-state
  workloads;
- replaces busy-wait polling during shutdown with bounded cond-var
  waits;
- removes the redundant `srv->sublist_lock` so publish and teardown
  no longer serialise against themselves.

Real workloads that should expose the wins:

- High-fanout (`examples/benchmark` with `-c 100`+ and many subscribers);
- Large payloads closer to 64 KiB so the mpool path is exercised;
- Long-running servers with frequent client churn that exercises the
  destroy shutdown path;
- Multi-worker configurations where cross-thread mailbox send/recv
  happens per delivery.

A more aggressive benchmark that exposes those paths is left for a
follow-up; the comparison would need to add a multi-subscriber
publisher and a multi-worker config knob to `examples/benchmark`.