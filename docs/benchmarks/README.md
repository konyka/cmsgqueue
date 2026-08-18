# CMSGQueue Performance Benchmarks

## Canonical workload

`./examples/benchmark -c 10 -n 10000 -t 1 [-j]`

10 publishers, 1 subscriber, 10 000 64-byte messages, single worker thread.
Release build (`-DCMAKE_BUILD_TYPE=Release`).

## Headline metrics (v0.5.1 final, 5-run mean)

| metric | mean | floor (P9 gate) |
|---|---|---|
| msg/s (end-to-end) | 31 300 | ≥ 25 000 |
| p50 inter-arrival (µs) | 50 | — |
| p99 inter-arrival (µs) | 99 | ≤ 200 |
| avg latency (ms) | 0.030 | — |
| dropped / 10 000 | ~2 400 | ≤ 5 000 |

The 25K-msg/s floor is ~25 % below the v0.5.0 baseline (33 784 msg/s).
The 200-µs p99 floor is roughly 2× the measured p99. Together they
catch material regressions without flaking on lower-resource CI
runners.

## Recorded per-run transcripts

- `docs/benchmarks/after_*.txt` (5 runs) — v0.5.0 baseline at
  commit `cbc5cc9`.
- `docs/benchmarks/v051p9_*.txt` (5 runs) — v0.5.1 P9 bootstrap.
- `docs/benchmarks/v051final_*.txt` (5 runs) — v0.5.1 final
  (after P9 consolidation).

## JSON mode (`-j`)

`-j` appends a single machine-readable JSON line to stdout:

```json
{"msg_per_sec":33428,"p50_us":50.0,"p99_us":99.0,"received":7563,"sent":10000,"dropped":2437,"clients":10,"threads":1,"n":10000}
```

Used by `tests/test_bench_regression.c` to gate regressions.

## P9 perf-regression gate

Enable at configure time:

```bash
cmake -S . -B build-bench -DCMQ_ENABLE_BENCH=ON
cmake --build build-bench -j
cd build-bench && ctest -V -L BENCH
```

Excluded from default CTest (`LABELS=BENCH`) to keep CI stable on
resource-constrained runners. PRs that touch the hot path must include
a bench comparison in the commit body; regressions within 5 % of the
recorded baseline are accepted, regressions > 5 % block merge unless
the lead approves.

See `docs/adr/0017-perf-regression-gate.md` for the full rationale.