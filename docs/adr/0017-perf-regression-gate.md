# ADR 0017 — Performance Regression Gate (P9)

**Status.** Accepted (v0.5.1).
**Date.** 2026-08-18.
**Context.** v0.5.0 release had no monotonic perf gate. Every PR landed without
a publish-path throughput/latency assertion; the only signal came from
`docs/benchmarks/results.md` (5-run hand-transcribed transcripts). The
adversarial v0.5.1 review (Round 2 cross-attack on `v0.5.1.perf.md`) flagged
that none of the proposed fixes would fail a perf regression because no
RED-style gate existed.

## Decision

Adopt an **opt-in, machine-readable, monotonic** performance gate:

1. **`examples/benchmark`** (`-c 10 -n 10000 -t 1`) is the canonical workload.
   It now prints **p50, p99, msg/s, dropped** in a single JSON line on stdout
   (mode flag `-j`) plus the existing human-readable summary.
2. **`tests/test_bench_regression.c`** is a small RED-style test that runs
   `examples/benchmark` as a child process, parses the JSON output, and
   asserts hard floors:
   - msg/s ≥ 30 000
   - p99 publish latency ≤ 60 µs
   - dropped == 0
3. **Opt-in build option** `CMQ_ENABLE_BENCH=ON` (off by default). When
   disabled, `test_bench_regression.c` is excluded from the default CTest
   glob (CMake `LABELS=BENCH`). When enabled, CTest picks it up.
4. **Baseline record.** `docs/benchmarks/results.md` is the canonical baseline.
   PR bodies that touch hot-path code must include before/after JSON output.
5. **Recorded-baseline + floor dual check.** PRs that intentionally improve
   a metric re-record the baseline in the same PR. Perf commits that regress
   within 5 % of the recorded baseline are accepted; regressions > 5 % block
   merge unless the lead (sisyphus) approves in writing.

## Rationale

The 30K-msg/s floor is ~10 % below the v0.5.0 baseline (33 784 msg/s), giving
room for measured noise without flaking CI. The p99 ≤ 60 µs floor is roughly
2× the v0.5.0 average latency (29 µs) — tight enough to catch the
250-500 µs synchronous-WAL stall that the adversarial review identified as
the biggest perf win, loose enough to avoid CI flake. Together the two
floors are sufficient to gate P0/P1/P2 items.

Opt-in (`CMQ_ENABLE_BENCH`) keeps the default CTest stable on
lower-resource CI runners and prevents `test_bench_regression.c` from being
a CI flake source. The default test count (55) is unaffected.

## Consequences

- Any future PR that touches `src/server/cmq_server.c`, `cmq_filestore.c`,
  `cmq_route.c`, `cmq_sublist.c`, `cmq_account.c`, `cmq_subject_rl.c`,
  `cmq_quota.c`, or `cmq_parser.c` MUST include a bench comparison in the
  commit body.
- The perf review member (currently `unspecified-high-1`) reviews those
  comparisons on demand. The lead approves when comparison shows > 5 %
  regression.
- The benchmark JSON format is stable; downstream tooling may parse it.

## Alternatives Considered

- **Always-on gate** (run bench on every CI). Rejected: `ctest -j1` already
  takes 4-6 s; bench adds 30 s + noise.
- **Separate bench/ subdir with multiple scenarios**. Rejected for v0.5.1:
  single workload covers the hot-path regression case. P4/P5/P8 items will
  add scenario-specific benches later (Phase 4 consolidation).
- **Compare against recorded baseline only (no absolute floor)**. Rejected:
  absolute floor catches under-tuned machines where the recorded baseline
  was itself a regression.