# CMSGQueue — Final Implementation Plan

**Generated.** Hyperplan 3-round adversarial review (5 category members × 3 rounds + 1 plan agent).
**Repo.** `/home/timeshift/opensource/cmsgqueue`. HEAD `715c9fc`. Remote `origin git@github.com:konyka/cmsgqueue.git` (fetch + push).
**Bundle inputs.** `docs/reviews/hyperplan-bundle.md` (input); `docs/reviews/round2_deep_attack.md`; `docs/reviews/round2_perf_attack.md`; Round 1 evidence under `tmp/opencode/cmsgqueue-deep-report.md`.
**Constraint envelope.** C11, NATS-like, performance-first, secure, reliable, TDD, atomic commits, push to remote.

---

## Part 1 — Plan Overview

### Goal
Implement, test, benchmark, document, and ship the 15 features (F1–F15) catalogued in the bundle. **Close the wire interop bug (F11) first** — it is currently a silent interop bug where compressed PUBLISHes round-trip as plaintext garbage to every subscriber. Land every feature as a per-PR, TDD-first workstream that honors the bundle §3.2 order.

### Total scope (estimated)

| Bucket | Count |
|---|---|
| Source files created | ~24 |
| Source files modified | ~9 |
| New tests | 15 (`tests/test_<name>.c`) + 2 fuzz harnesses |
| New benches | 4 |
| PRs | **15** (one per feature) + 1 prep + 1 final docs/push = **17 PRs** |
| Docs | 24 (13 feature + 6 ADR + 5 architecture) |
| Final tag | `v0.2.0` |
| Approx LOC added | ~3 500 impl + ~2 800 tests + ~1 600 docs |

### Critical path
**F11 → F7 → F8 → F9 → F3 → F2 → F4 → F1 → F5 → F15 → F10 → F12/F13** (F6 parallel to F5).

This sequence gates every other feature: TLS (F1) cannot be exercised without `CMQ_OP_INFO` knowing the server's TLS posture; persistence (F5) cannot be benchmarked until the wire is safe; auth (F8) blocks the security banner needed before any wire-level feature ships.

### Risk register (abridged)

| ID | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | F11 breaks existing test fixtures that set flag bits | M | M | `grep "flags =" tests/` before merge; fix fixture, not parser |
| R2 | Hardening slows hot path (`-fstack-protector-strong`) | H | M | Per-file exclusion list (`cmq_parser.c`, `cmq_slab.c`, `cmq_mpool.c`) |
| R3 | F1 OpenSSL not available on CI | M | H | Drop `find_package(OpenSSL QUIET)`; hard-fail; gate TLS feature on OpenSSL |
| R4 | argon2id CPU flood (auth brute-force) | M | M | Per-IP rate limit on CONNECT (F10) |
| R5 | hw-CRC unavailable on riscv/old-arm | M | L | Software fallback guaranteed; flag clearly |
| R6 | Persistence recovery >10 s for 1 GB store | M | M | Snapshot every N records; truncate-on-recovery cap |
| R7 | MQTT 5.0 property gaps | M | M | Document v0.2.0 supports property subset; roadmap remaining |
| R8 | zstd absent in CI | M | L | Vendor zstd under `third_party/zstd/`, single-file amalgamation |
| R9 | Parser regression for old clients (cfbae48 forward) | M | H | Backward compat: future flags in `INFO`; fall through unknown flags pre-INFO |
| R10 | SSH key for push not configured | M | H | Verify `ssh -T git@github.com` before push; user instructions in commit plan |
| R11 | CI time budget blown by full sanitizer+coverage matrix | M | M | Shard by feature; per-PR CI runs only changed tests + full smoke |

---

## Part 2 — Feature Plans (F1–F15)

Format: **Complexity** · **Files** · **Tests** · **Benchmark** · **Docs** · **Verification gates** · **Commit message** · **Risk** · **Parallel with**.

### F1 — TLS (OpenSSL backend)

- **Complexity:** XL.
- **Files touched:**
  - `src/enterprise/cmq_tls.c:161-164` — replace stub `cmq_tls_backend_secure()` returning 0 with real SSL_CTX_factory.
  - `src/enterprise/cmq_tls.h` — add `cmq_tls_per_listener_create()` API.
  - `src/server/cmq_server.c` — accept-loop integration (per-listener CTX), SNI/ALPN hook, hot-reload via `SSL_CTX_up_ref`.
  - `src/server/cmq_config.c` — add `tls_cert`, `tls_key`, `tls_ca`, `tls_min_version`, `tls_ciphers` config keys.
  - `CMakeLists.txt:159-164` — drop `find_package(OpenSSL QUIET)`; hard-fail when TLS requested + OpenSSL missing.
- **Tests added:**
  - `tests/test_tls.c` — handshake, cipher policy, AEAD-only, SNI, session tickets, `openssl s_client` end-to-end.
  - `tests/test_tls_reload.c` — `SSL_CTX_up_ref` hot-swap.
  - `tests/test_tls_failure.c` — bad cert, expired cert, wrong CA, handshake timeout.
- **Benchmark added:** `docs/benchmarks/tls.md` — connections/sec with TLS 1.3 vs 1.2, handshake latency, throughput vs plaintext.
- **Docs added:** `docs/features/tls.md` + `docs/architecture/tls.md` + `docs/adr/0005-per-listener-tls-ctx.md`.
- **Verification gates:**
  - `openssl s_client` connects successfully.
  - `cmq_tls_backend_secure()` returns 1.
  - A TLS 1.0 client is rejected.
  - Cipher suite restricted to AEAD (verified via `SSL_CTX_get_ciphers`).
  - Hot reload does not drop existing connections.
- **Commit message:** `feat(enterprise): TLS 1.2/1.3 per-listener backend with OpenSSL`
- **Risk:** R2 (compiler overhead), R3 (OpenSSL ABI).
- **Parallel with:** F2, F3, F9, F10, F12, F13.

### F2 — Wire compression (CMQ_FLAG_COMPRESSED, BATCH-level)

- **Complexity:** L.
- **Files touched:**
  - `src/proto/cmq_proto.h:14` — flag already defined; document as BATCH-ONLY.
  - `src/proto/cmq_parser.c:274` — reject flag on non-BATCH frames (send error pre-CONNACK fallback).
  - `src/server/cmq_server.c:3931-3990` (`handle_batch`) — when flag set, zstd-decompress payload before fan-out.
  - New `src/proto/cmq_compress.c` — zstd wrapper with stream API.
  - New `third_party/zstd/` — single-file amalgamation vendor.
  - `CMakeLists.txt` — link zstd when compile flag set.
- **Tests added:**
  - `tests/test_compress.c` — round-trip, ratio threshold, error on malformed input, decompression-bomb cap.
  - `tests/test_compress_auto_skip.c` — payloads <512 B pass through uncompressed.
- **Benchmark added:** `docs/benchmarks/compress.md` — zstd-1 ratio on 1 KiB JSON, throughput regression on 64 B medians.
- **Docs added:** `docs/features/compression.md` + `docs/adr/0004-batch-level-compression.md`.
- **Verification gates:**
  - Round-trip preserves bytes.
  - Auto-skip below 512 B.
  - Decompression bomb cap at 16× uncompressed size.
  - Compression flag on non-BATCH frame is rejected.
- **Commit message:** `feat(proto): CMQ_FLAG_COMPRESSED batch-level via zstd`
- **Risk:** Regresses 64 B median if not auto-skipped.
- **Parallel with:** F1, F3, F9, F10, F12, F13.

### F3 — Wire checksum (CMQ_FLAG_CHECKSUM)

- **Complexity:** M.
- **Files touched:**
  - `src/proto/cmq_parser.c:274` — verify checksum in `cmq_parser_feed` after frame complete.
  - `src/server/cmq_server.c` — compute checksum on outbound MESSAGE when peer advertised support.
  - New `src/core/cmq_crc32c_hw.c` — Intel SSE4.2 + ARM CRC32 intrinsics.
  - New `src/core/cmq_crc32c_sw.c` — software fallback.
  - `cmake/cmq_compiler.cmake` — detect `CMQ_HAVE_SSE4_2`, `CMQ_HAVE_ARM_CRC32`.
- **Tests added:**
  - `tests/test_checksum.c` — IEEE 802.3 reference vectors, salt vectors, random fuzz.
  - `tests/test_checksum_wire.c` — bit-flip mid-frame triggers `cmq_send_error("checksum mismatch")`.
- **Benchmark added:** `docs/benchmarks/checksum.md` — hw-accelerated vs software throughput.
- **Docs added:** `docs/features/checksum.md`.
- **Verification gates:**
  - Reference vector test passes.
  - Bit-flip rejection in wire test.
  - Hot-path overhead ≤5% on 64 B payload.
- **Commit message:** `feat(proto): CMQ_FLAG_CHECKSUM crc32c verification (hw-accelerated)`
- **Risk:** R5 (hw unavailable).
- **Parallel with:** F1, F2, F9, F10, F12, F13.

### F4 — `CMQ_OP_INFO` emission

- **Complexity:** M.
- **Files touched:**
  - `src/proto/cmq_proto.h` — already defined; document payload format.
  - `src/server/cmq_server.c` — new `handle_info()`, send INFO at handshake (after CONNECT) and on config reload.
  - `src/server/cmq_server.h` — INFO payload struct.
  - `examples/benchmark.c` — example consumer.
- **Tests added:**
  - `tests/test_info.c` — verify INFO fields: server_id, version, max_payload, go_park_time, auth_required, tls_required, compression_codecs.
  - `tests/test_info_post_reload.c` — INFO resent on config change.
- **Benchmark added:** N/A (one-shot handshake).
- **Docs added:** `docs/architecture/wire-protocol.md` (INFO section).
- **Verification gates:**
  - INFO receives within 100 ms of CONNECT.
  - All advertised fields present.
  - No regression on existing clients.
- **Commit message:** `feat(server): emit CMQ_OP_INFO at handshake with capability bitmap`
- **Risk:** R9 (old clients).
- **Parallel with:** F1, F2, F3, F9, F10, F12, F13.

### F5 — Persistence wired into server

- **Complexity:** XL.
- **Files touched:**
  - `src/server/cmq_server.c:6218` — wire `cmq_store` into `cmq_server_create`.
  - `src/server/cmq_server.c` — new `cmq_persist_init()`, `cmq_persist_wal_append()`, `cmq_persist_wal_recover()`.
  - `src/store/cmq_filestore.c` — already 719 LOC; add group-commit fsync.
  - `src/store/cmq_filestore.h` — new constants.
  - `src/server/cmq_config.c` — `persist_dir`, `persist_wal_max_records`, `persist_sync_mode` config keys.
- **Tests added:**
  - `tests/test_persist_wire.c` — simple publish with `persist=async`, restart, verify recovery.
  - `tests/test_persist_crash.c` — kill -9 mid-write, restart, verify tail replay.
  - `tests/test_persist_group_commit.c` — verify fsync amortization.
- **Benchmark added:** `docs/benchmarks/persistence.md` — throughput vs sync mode, recovery time vs store size.
- **Docs added:** `docs/features/persistence.md` + `docs/architecture/persistence.md` + `docs/adr/0006-persistence-wal-snapshot.md`.
- **Verification gates:**
  - Recovery time ≤10 s for 1 GB store.
  - fsync ≤5 ms amortized (group-commit).
  - No message loss under sync mode (kill -9 + restart).
  - Existing tests still pass (no regression on streaming consumer pattern).
- **Commit message:** `feat(server): wire filestore into server with WAL + group-commit fsync`
- **Risk:** R6 (recovery time).
- **Parallel with:** F6, F10, F12, F13.

### F6 — MQTT 5.0 bridge wiring

- **Complexity:** L.
- **Files touched:**
  - `src/server/cmq_server.c` — add MQTT listener on separate port (default 1883).
  - `src/enterprise/cmq_mqtt.c` — already 665 LOC; wire into `cmq_server_create`.
  - `src/server/cmq_config.c` — `mqtt_enabled`, `mqtt_port`, `mqtt_max_clients`.
- **Tests added:**
  - `tests/test_mqtt_publish_qos0.c` — QoS 0 round-trip.
  - `tests/test_mqtt_publish_qos1.c` — QoS 1 PUBACK.
  - `tests/test_mqtt_property_subset.c` — property mapping correctness.
  - `tests/test_mqtt_to_cmq.c` — MQTT->CMQ routing.
- **Benchmark added:** `docs/benchmarks/mqtt.md` — end-to-end throughput.
- **Docs added:** `docs/features/mqtt-bridge.md`.
- **Verification gates:**
  - Round-trip with `mosquitto_pub` / `mosquitto_sub`.
  - Property mapping matches spec.
  - Existing MQTT library tests still pass.
- **Commit message:** `feat(enterprise): wire MQTT 5.0 bridge into server listener`
- **Risk:** R7 (property gaps).
- **Parallel with:** F5, F10, F12, F13.

### F7 — Build hardening

- **Complexity:** S.
- **Files touched:**
  - `cmake/cmq_compiler.cmake` — add `-D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -pie -Wl,-z,relro,-Wl,-z,now`.
  - `cmake/cmq_compiler.cmake` — exclusion list file pragma for `cmq_parser.c`, `cmq_slab.c`, `cmq_mpool.c` (no `-fstack-protector-strong`).
  - `.github/workflows/` — if hardening build added, label `hardening`.
- **Tests added:** `tests/test_hardening.c` — compile-flag assertion (or `tests/cmq_build.sh`).
- **Benchmark added:** `docs/benchmarks/hardening.md` — before/after throughput.
- **Docs added:** `docs/adr/0001-build-hardening.md`.
- **Verification gates:**
  - `readelf -dW build/cmq_server | grep -E 'BIND_NOW|FLAGS'` shows hardening flags.
  - All existing tests pass.
  - Throughput regression ≤2%.
- **Commit message:** `chore(build): enable FORTIFY, PIE, RELRO, partial stack-protector-strong`
- **Risk:** R2.
- **Parallel with:** F8, F9, F10, F12, F13.

### F8 — Auth password hashing (argon2id)

- **Complexity:** M.
- **Files touched:**
  - `src/server/cmq_config.c:124-125` — replace `strdup` plaintext with `libsodium` `crypto_pwhash_str`.
  - `CMakeLists.txt` — link libsodium when `CMQ_ENABLE_ARGON2=ON`.
  - `cmake/cmq_compiler.cmake` — detect libsodium.
  - `examples/cmq.conf` — example hashed password generation tool.
- **Tests added:**
  - `tests/test_auth_basic.c` — connect with correct password succeeds.
  - `tests/test_auth_wrong.c` — connect with wrong password fails.
  - `tests/test_auth_argon_verify.c` — verify integration with libsodium.
  - `tests/test_auth_plaintext_rejected.c` — server refuses startup with plaintext-only password config.
- **Benchmark added:** `docs/benchmarks/auth.md` — verify time per attempt.
- **Docs added:** `docs/features/auth.md` + `docs/adr/0002-argon2id-auth.md`.
- **Verification gates:**
  - argon2id verify costs ~64 MiB / ~100 ms (configurable).
  - Plaintext-only config refused on startup.
  - Backward-compatible: if config file has plaintext, server emits WARN + refuses.
  - Existing auth tests pass.
- **Commit message:** `feat(auth): argon2id password hashing, refuse plaintext-only config`
- **Risk:** R4 (CPU flood).
- **Parallel with:** F7, F9, F10, F12, F13.

### F9 — Hardware CRC32C

- **Complexity:** M.
- **Files touched:**
  - New `src/core/cmq_crc32c_hw.c` — `_mm_crc32_u64` (x86_64) + `crc32cx` (aarch64).
  - New `src/core/cmq_crc32c_sw.c` — software fallback.
  - `src/core/cmq_crc32c.h` — public API.
  - `src/store/cmq_filestore.c:54-65` — replace software loop with hw-accelerated.
  - `cmake/cmq_compiler.cmake` — detect `CMQ_HAVE_SSE4_2`, `CMQ_HAVE_ARM_CRC32`.
- **Tests added:**
  - `tests/test_crc32c.c` — IEEE 802.3 reference vectors, salt vectors, random fuzz, hw/sw equivalence.
- **Benchmark added:** `docs/benchmarks/crc32c.md` — hw vs sw throughput.
- **Docs added:** `docs/adr/0003-hw-crc32c.md`.
- **Verification gates:**
  - Reference vector test passes on hw and sw.
  - Hot-path overhead ≤5% on 64 B payload (with hw).
  - Software fallback compiles on all CI platforms.
- **Commit message:** `perf(core): hw-accelerated CRC32C with software fallback`
- **Risk:** R5.
- **Parallel with:** F1, F2, F3, F10, F12, F13.

### F10 — Rate limiting (per-IP, per-subject)

- **Complexity:** M.
- **Files touched:**
  - New `src/server/cmq_ratelimit.c` — token bucket per IP, per-subject cardinality cap.
  - `src/server/cmq_server.c` — accept-loop integration, sublist integration.
  - `src/server/cmq_config.c` — `ratelimit_ip_per_sec`, `ratelimit_subjects_per_conn`.
- **Tests added:**
  - `tests/test_ratelimit_ip.c` — load test, verify throttle.
  - `tests/test_ratelimit_subject.c` — verify cardinality cap.
  - `tests/test_ratelimit_burst.c` — verify burst allowance.
- **Benchmark added:** `docs/benchmarks/ratelimit.md` — overhead per request.
- **Docs added:** `docs/features/ratelimit.md`.
- **Verification gates:**
  - Rate limit kicks in at threshold.
  - Subject cap rejects 1025th subject.
  - Reset works on disconnect.
- **Commit message:** `feat(server): per-IP rate limiting and per-conn subject cardinality cap`
- **Risk:** None significant.
- **Parallel with:** F1, F2, F3, F5, F6, F9, F12, F13.

### F11 — Reject unknown wire flags (interop bug fix)

- **Complexity:** S.
- **Files touched:**
  - `src/proto/cmq_parser.c:274` — inspect flags byte, reject bits 0/1/2 until implemented.
  - `src/proto/cmq_proto.h` — document reserved flags.
- **Tests added:**
  - `tests/test_flag_reject.c` — flag 0x01 rejected pre-CONNACK, flag 0x02 rejected pre-CONNACK.
  - `tests/test_flag_unknown.c` — flag 0xFC rejected.
- **Benchmark added:** N/A.
- **Docs added:** `docs/features/flags.md` (or `docs/architecture/wire-protocol.md` update).
- **Verification gates:**
  - All three flag bits rejected until feature ships.
  - No regression on existing tests (scan fixtures for stray flag bits).
- **Commit message:** `fix(proto): reject unknown CMQ_FLAG_* bits pre-CONNACK (closes interop bug)`
- **Risk:** R1.
- **Parallel with:** F7, F8, F9, F10, F12, F13.

### F12 — Health endpoint `/healthz`, `/readyz`

- **Complexity:** S.
- **Files touched:**
  - `src/server/cmq_server.c` — new HTTP listener on separate port (default 8222).
  - `src/server/cmq_config.c` — `health_enabled`, `health_port`.
- **Tests added:**
  - `tests/test_health.c` — `/healthz` returns 200, `/readyz` returns 503 when draining.
- **Benchmark added:** N/A.
- **Docs added:** `docs/features/health.md`.
- **Verification gates:**
  - `/healthz` returns 200.
  - `/readyz` returns 200/503 correctly.
- **Commit message:** `feat(server): /healthz and /readyz endpoints`
- **Risk:** None.
- **Parallel with:** F1, F2, F3, F5, F6, F9, F10, F13.

### F13 — Prometheus metrics

- **Complexity:** M.
- **Files touched:**
  - New `src/server/cmq_metrics.c` — Prometheus-format exporter.
  - `src/server/cmq_server.c` — wire on health port `/metrics`.
  - `src/server/cmq_server.h` — stats struct.
- **Tests added:**
  - `tests/test_metrics.c` — verify expected metrics emitted.
- **Benchmark added:** N/A.
- **Docs added:** `docs/features/metrics.md`.
- **Verification gates:**
  - `/metrics` returns 200 + text/plain.
  - Includes: connections, msgs in/out, subscriptions, latency histograms.
- **Commit message:** `feat(server): Prometheus /metrics exporter`
- **Risk:** None.
- **Parallel with:** F1, F2, F3, F5, F6, F9, F10, F12.

### F14 — Software CRC portability test

- **Complexity:** S.
- **Files touched:**
  - `tests/test_crc32c_portable.c` — run on every CI platform.
- **Tests added:** matches F9 + endianness coverage.
- **Benchmark added:** N/A.
- **Docs added:** Tied to F9.
- **Verification gates:** Cross-platform passing.
- **Commit message:** `test(core): crc32c portability coverage`
- **Risk:** None.
- **Parallel with:** F1, F2, F3, F5, F6, F9, F10, F12, F13.

### F15 — REQUEST/RESPONSE inbox HoL fix

- **Complexity:** M.
- **Files touched:**
  - `src/proto/cmq_parser.c:60-67` — separate inbox budget.
  - `src/server/cmq_config.c` — `inbox_max_payload`, `inbox_max_pending`.
  - `src/server/cmq_server.c` — flows into sublist and queue groups.
- **Tests added:**
  - `tests/test_request_inbox.c` — slow responder does not block healthy publisher.
  - `tests/test_request_inbox_budget.c` — inbox overflow sends error.
- **Benchmark added:** `docs/benchmarks/request-reply.md` — under mixed load.
- **Docs added:** `docs/features/request-reply.md`.
- **Verification gates:**
  - Slow responder does not affect other publishers on same conn.
  - Inbox budget errors are observable.
- **Commit message:** `feat(server): REQUEST/RESPONSE independent inbox budget`
- **Risk:** None significant.
- **Parallel with:** F5, F6, F10, F12, F13.

---

## Part 3 — Execution Sequence

### Phase 1 (parallel, day 1-2) — interop fix + cheap wins
- **F11** (reject unknown flags) — 1 file, 1 day.
- **F7** (build hardening) — 1 file, 1 day.
- **F9** (hw-CRC) — 3 files, 1 day.

### Phase 2 (parallel, day 3-5) — security baseline
- **F8** (argon2id auth) — 2 files, 2 days.
- **F10** (rate limit) — 3 files, 2 days.
- **F12** (health) — 2 files, 1 day.
- **F13** (metrics) — 2 files, 2 days.
- **F14** (CRC portability test) — 1 file, 1 day.

### Phase 3 (parallel, day 6-10) — wire-level features
- **F3** (checksum) — gated on F9, 3 files, 2 days.
- **F2** (compression) — 4 files, 3 days.
- **F4** (CMQ_OP_INFO) — 2 files, 2 days.

### Phase 4 (parallel, day 11-20) — heavy features
- **F1** (TLS) — 5 files, 7 days.
- **F5** (persistence) — 3 files, 6 days.
- **F6** (MQTT) — 3 files, 4 days.
- **F15** (inbox HoL) — 3 files, 2 days.

### Phase 5 (day 21-22) — final docs and push
- Generate `CHANGELOG.md` from all PRs.
- Update `README.md` with new feature list.
- Final smoke run on full matrix.
- Tag `v0.2.0`.
- Push to remote.

---

## Part 4 — Verification Gates

### Per-phase gates
- **Phase 1 → 2:** All Phase 1 PRs merged to `main`. CI green.
- **Phase 2 → 3:** Baseline benchmark re-run; throughput regression ≤5%.
- **Phase 3 → 4:** All wire-level tests pass; no parser regressions.
- **Phase 4 → 5:** Full feature matrix passing on all CI platforms.

### Per-feature gates (each feature must satisfy to merge)
- Tests written first (Red), then passing (Green), then refactored.
- Benchmark captures before/after.
- Doc added matching the template.
- Commit message follows Conventional Commits.
- PR has linked issue.
- All CI checks green: ctest, ASan, UBSan, TSan (where applicable), coverage ≥80% on new code.

### Final gates (before push)
- All 15 PRs merged.
- `main` branch green on full matrix.
- `CHANGELOG.md` updated.
- `docs/` complete.
- `v0.2.0` tag created locally.
- `git remote -v` shows `origin` configured (already confirmed: `git@github.com:konyka/cmsgqueue.git`).
- SSH key verified: `ssh -T git@github.com` returns success.

---

## Part 5 — Commit & Push Plan

### Branch strategy
- `main` is the only protected branch.
- Each feature is a feature branch `feat/<name>` (e.g., `feat/tls`, `feat/persistence`).
- One PR per feature → squash/merge to `main`.
- After all 15 PRs merged, `main` is the `v0.2.0` candidate.

### "Tests pass" definition
A PR is mergeable when ALL of:
1. `ctest --output-on-failure -j$(nproc)` green on the PR's CI run.
2. Sanitizers (ASan, UBSan, TSan) green on Linux.
3. Coverage gate ≥80% on new code (per-PR only).
4. `lint:clang-tidy` (if configured) green.
5. Markdown link-check green for docs added.
6. Reviewer approval (1 from project owner).

### Push trigger
**Sequential per-PR.** Each PR is merged and pushed to `main`. After Phase 5, on `main`:
```bash
git tag -a v0.2.0 -m "v0.2.0: TLS, persistence, MQTT, wire compression/checksum, build hardening"
git push origin v0.2.0
git push origin main
```

### Remote config (verified)
- **Remote:** `origin git@github.com:konyka/cmsgqueue.git` (fetch + push).
- **SSH key:** User must verify `ssh -T git@github.com` works before push. If not, see https://docs.github.com/en/authentication.
- **Push URL:** `ssh://git@github.com/konyka/cmsgqueue.git` will be used by git automatically.

---

## Part 6 — Open Questions for the User

1. **CI matrix:** The repo has `.github/workflows/`. Confirm: should we add `hardening` label + `coverage` job? Currently `2c1e0a5 ci: add TSan and coverage jobs`. So coverage might already be present. Verify before adding.
2. **Dep choice:** zstd vs LZ4 — bundle picked zstd but with LZ4 fallback. Confirm zstd is acceptable as a new dependency.
3. **CMQ_OP_INFO:** Is this needed immediately, or can we defer? The plan agent argues YES for TLS (F1 needs it), but old clients work without it. If deferred, F1's perf claims weaken.
4. **v0.2.0 scope:** Should F12/F13 (health/metrics) be in v0.2.0 or v0.2.1? They're cheap but expandable.
5. **libsodium OK?** Adding a new dependency for argon2id. Alternative: implement PBKDF2 from OpenSSL (no new dep). Trade-off: argon2id is stronger, but PBKDF2+EVP is in-tree.
6. **Persistence always-on or opt-in?** If always-on, users without a disk expect failures. Default to opt-in with explicit config.

---

## Part 7 — Provenance

This plan was produced by the **`plan` agent at MiniMax-M3**, after a 3-round adversarial hyperplan review:
- **Round 1 (independent analysis):** 5 agents (unspecified-low, unspecified-high, ultrabrain, artistry, deep).
- **Round 2 (cross-attack):** Each agent attacked the other 4's findings.
- **Round 3 (defense/refinement):** Conceded/refined 18 convergent critiques.
- **Bundle:** `docs/reviews/hyperplan-bundle.md` (this repo).
- **Original adversarial reports:** `docs/reviews/round2_*.md`, `tmp/opencode/cmsgqueue-deep-report.md`.

The plan agent's full output is captured in this file. The bundle's 12 defensible decisions survived 2 rounds of hostile review. Implementation order in §3.2 of the bundle is honored in Phase 1–4 above.

---

## Part 8 — Top-Level Acceptance Criteria

The plan is complete when:
- [ ] All 15 features merged to `main`.
- [ ] All per-feature verification gates met.
- [ ] CI green on linux gcc/clang, macOS, Windows, ARM64.
- [ ] `CHANGELOG.md` reflects v0.2.0.
- [ ] `v0.2.0` tag pushed.
- [ ] `git log --oneline v0.2.0` shows commits across all 15 features.
- [ ] `git remote -v` confirms successful push.
- [ ] All TODOs/FIXMEs removed from source (or moved to GitHub issues with cross-refs).
