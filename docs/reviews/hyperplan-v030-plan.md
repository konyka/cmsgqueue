# CMSGQueue v0.2.0 → v0.3.0 — Final Executable Plan

**Provenance.** 3-round hyperplan review (3 of 5 agents returned, 2 timed out; lead-orchestrator synthesis). Source: v0.2.0 (HEAD c386ad4, 36/36 tests, 11 features shipped).
**Constraint envelope.** C11, NATS-like, performance-first, secure, reliable, TDD, atomic commits, push to `origin main`.
**Bundle source.** `docs/reviews/hyperplan-v030-bundle.md`.

---

## Part 1 — Plan Overview

### Goal
Ship v0.3.0 with **13 features** (F8, F8b, F11–F21) merged behind TDD-tested, benchmarked, documented PRs. Close wire-security gaps first (F8 auth, F11 trace, F8b rate-limit). Defer F19 (server-side MQTT listener) to v0.4.0. Final tag `v0.3.0`, pushed to `origin main` after full CI matrix green.

### Total scope

| Bucket | Count |
|---|---|
| Source files created | ~18 |
| Source files modified | ~6 |
| New tests | 13 + 3 fuzz harnesses |
| New benches | 4 |
| PRs | 13 (one per feature) + 1 docs/push = **14 PRs** |
| Docs | 13 in `docs/features/` + 3 ADRs |
| Final tag | `v0.3.0` |
| Approx LOC added | ~2 800 impl + ~2 400 tests + ~1 100 docs (~6 300 total) |

### Critical path
```
F8 (scrypt auth) → F8b (auth rate-limit) → F11 (trace IDs) → F12 (TLS hardening)
F18 (persistent subs) — uses existing F5 WAL only, no F5 rework.
Independent parallel branches after F11:
  • F13 (audit) ‖ F14 (quota) ‖ F15 (blocklist) ‖ F16 (ACL) ‖ F17 (inter-node TLS)
F20 (coverage+fuzz) ‖ F21 (pre-commit) at the END as quality gates.
```

### Perf budget
- End-to-end msg/s: ≤5% regression from v0.2.0 baseline (29,234 msg/s, 34 µs)
- Auth (CONNECT): ≤120 ms (scrypt verify)
- Startup recovery: O(N) parallelized at 100K+ records
- Memory @ 100K subs: ≤16 MB
- TLS handshake: ≤5 ms (cached)

### Risk register
- **R1**: F8 scrypt blocks accept thread (100ms). Mitigation: per-IP auth rate-limit (F8b).
- **R2**: F18 persistent subs block startup. Mitigation: parallelize load + snapshot.
- **R3**: F17 inter-node TLS adds CPU. Mitigation: AES-GCM-NI hw acceleration.
- **R4**: F20 coverage gate blocks PRs. Mitigation: per-feature threshold, not global.
- **R5**: F14 quota enforcement reject vs drop. Decision: reject-with-error.

---

## Part 2 — Feature Plans

### F8 — Password hashing (OpenSSL scrypt)

- **Complexity:** M.
- **Files:** `src/server/cmq_password.{h,c}` (new), `src/server/cmq_config.{h,c}`, `src/server/cmq_server.c:4395-4429` (verify path).
- **Tests added:** `tests/test_password.c` — round-trip, format parser, malformed, against fixtures.
- **Benchmark added:** `bench/bench_password.c` — verify time @ N=2^14, r=8, p=1.
- **Docs added:** `docs/features/password-hash.md` + `docs/adr/0007-scrypt-auth.md`.
- **Verification gates:**
  - Format prefix `$scrypt$N=2^14,r=8,p=1$<salt-b64>$<hash-b64>` parses.
  - Legacy plaintext path: stores under deprecation marker, warns once.
  - Hot-path: ≤120 ms per `EVP_PBE_scrypt`.
- **Commit message:** `feat(auth): scrypt password hashing with `$scrypt$...` format`
- **Parallelizable with:** F8b (depends), F11 (independent).

### F8b — Auth brute-force rate limit

- **Complexity:** S.
- **Files:** `src/server/cmq_rate_limit.c` (extend existing `rate_lock` for auth_attempts).
- **Tests added:** `tests/test_auth_ratelimit.c` — 10/sec cap, 11th attempt rejected.
- **Benchmark added:** existing `examples/benchmark` covers auth heat.
- **Docs added:** `docs/features/password-hash.md` (auth rate-limit section).
- **Verification gates:** 10 attempts/sec/IP allowed; 11th rejected with `cmq_send_connack(c, 4)`.
- **Commit message:** `feat(auth): brute-force rate limit on CONNECT (auth)`
- **Parallelizable with:** F11-F21 (independent of F8).

### F11 — Connection tracing (correlation IDs)

- **Complexity:** S.
- **Files:** `src/server/cmq_trace.{h,c}` (new), `src/server/cmq_server.c` (set on CONNECT).
- **Tests added:** `tests/test_trace.c` — unique IDs per connection, propagated to log.
- **Benchmark added:** none (16-byte UUID is hot-path negligible).
- **Docs added:** `docs/features/tracing.md`.
- **Verification gates:** trace ID is unique per connection; appears in `cmq_send_error` payload.
- **Commit message:** `feat(observability): per-connection trace IDs (16-byte UUID)`
- **Parallelizable with:** all features (independent).

### F12 — TLS hardening (per-listener, ALPN)

- **Complexity:** M.
- **Files:** `src/enterprise/cmq_tls.{h,c}` (split CTX per listener), `src/server/cmq_server.c` (listener registration).
- **Tests added:** `tests/test_tls_per_listener.c` — two listeners, distinct CTX, cipher policy.
- **Benchmark added:** `bench/bench_tls.c` — handshake latency.
- **Docs added:** `docs/features/tls-openssl.md` (update).
- **Verification gates:** distinct listeners have distinct SSL_CTX; ALPN "h2"/"http1.1" accepted.
- **Commit message:** `feat(tls): per-listener SSL_CTX with ALPN support`
- **Parallelizable with:** F11, F13-F21.

### F13 — Structured audit log

- **Complexity:** M.
- **Files:** `src/enterprise/cmq_audit.{h,c}` (new), `src/server/cmq_server.c` (event calls).
- **Tests added:** `tests/test_audit.c` — auth_ok/auth_fail/rate_limit_reject write JSON lines.
- **Benchmark added:** `bench/bench_audit.c` — write throughput.
- **Docs added:** `docs/features/audit.md`.
- **Verification gates:** audit log is JSON-lines; one event per line; events enumerated.
- **Commit message:** `feat(observability): structured audit log (JSON-lines)`
- **Parallelizable with:** F11, F14-F21.

### F14 — Per-account quota enforcement

- **Complexity:** M.
- **Files:** `src/enterprise/cmq_quota.{h,c}` (new), `src/server/cmq_account.c` (quota fields).
- **Tests added:** `tests/test_quota.c` — per-account msgs/sec + bytes/sec + connections cap.
- **Benchmark added:** `bench/bench_quota.c` — overhead per PUBLISH.
- **Docs added:** `docs/features/quota.md`.
- **Verification gates:** quota fields in `cmq_account`; enforcement at CONNECT increment and PUBLISH increment.
- **Commit message:** `feat(accounts): per-account quota enforcement (msgs/sec, bytes/sec, connections)`
- **Parallelizable with:** F11, F13, F15-F21.

### F15 — Connection blocklist

- **Complexity:** S.
- **Files:** `src/cluster/cmq_blocklist.{h,c}` (new), `src/server/cmq_server.c` (accept_cb check).
- **Tests added:** `tests/test_blocklist.c` — banned IP refused, reload API.
- **Benchmark added:** none (lookup is O(N) but small list).
- **Docs added:** `docs/features/blocklist.md`.
- **Verification gates:** banned IPs rejected pre-handshake; reload API documented.
- **Commit message:** `feat(security): IP blocklist for connect-time rejection`
- **Parallelizable with:** all features (independent).

### F16 — Per-subject ACL allow-list

- **Complexity:** M.
- **Files:** `src/enterprise/cmq_acl.{h,c}` (new), `src/server/cmq_account.c` (subject_allow/deny).
- **Tests added:** `tests/test_acl.c` — allow-list match, deny-list match, wildcards.
- **Benchmark added:** `bench/bench_acl.c` — per-publish overhead.
- **Docs added:** `docs/features/acl.md`.
- **Verification gates:** allow-list match; deny-list match; wildcard semantics; default deny if neither set.
- **Commit message:** `feat(accounts): per-subject ACL allow-list / deny-list`
- **Parallelizable with:** F11-F15, F17-F21.

### F17 — Inter-node encryption (cluster routes)

- **Complexity:** L.
- **Files:** `src/cluster/cmq_route_tls.{h,c}` (new), `src/cluster/cmq_route.c` (wrap socket).
- **Tests added:** `tests/test_route_tls.c` — encrypted handshake, cleartext rejected.
- **Benchmark added:** `bench/bench_route_tls.c` — msg/s with TLS vs plaintext.
- **Docs added:** `docs/features/route-tls.md` + `docs/adr/0009-inter-node-tls.md`.
- **Verification gates:** route connection encrypted; plaintext disabled by config flag.
- **Commit message:** `feat(cluster): TLS-wrapped route connections with shared secret`
- **Parallelizable with:** F11-F16, F18-F21. **Note:** touches `cmq_route.c`; serialize against other route changes.

### F18 — Persistent subscription state

- **Complexity:** L.
- **Files:** `src/server/cmq_sublist_persist.{h,c}` (new), `src/server/cmq_sublist.c` (mirroring).
- **Tests added:** `tests/test_sublist_persist.c` — write subs, restart, msg reaches only-live subs.
- **Benchmark added:** `bench/bench_sublist_persist.c` — startup recovery time.
- **Docs added:** `docs/features/persistent-subs.md` + `docs/adr/0008-persistent-subs.md`.
- **Verification gates:** subs survive restart; replay only delivers to live subs; cursor per sub.
- **Commit message:** `feat(server): persistent subscription state with WAL replay`
- **Parallelizable with:** F11-F17, F19-F21. **Depends on F5 (already shipped).**

### F19 — Server-side MQTT listener

- **Complexity:** XL.
- **Files:** `src/enterprise/cmq_mqtt_server.{h,c}` (new).
- **Tests added:** `tests/test_mqtt_listen.c` (minimal — CONNECT round-trip).
- **Benchmark added:** none.
- **Docs added:** `docs/features/mqtt-bridge.md` (update noting v0.4.0 defer).
- **Verification gates:** stub function `cmq_mqtt_server_listen()` returns `ENOSYS`.
- **Commit message:** `feat(enterprise): MQTT server-side listener stub (deferred to v0.4.0)`
- **Parallelizable with:** none (deferred — only stub).

### F20 — Coverage gate + fuzz tests

- **Complexity:** M.
- **Files:** `tests/fuzz/` (3 harnesses), `CMakeLists.txt` (coverage option).
- **Tests added:** fuzz harnesses for `cmq_parser_feed`, `cmq_mqtt_encode_publish`, `cmq_filestore_append`.
- **Benchmark added:** none.
- **Docs added:** `docs/features/coverage.md`.
- **Verification gates:** `CMQ_ENABLE_COVERAGE=ON` builds with `--coverage`; CI runs ≥80% gate.
- **Commit message:** `feat(quality): coverage gate + libfuzzer harnesses`
- **Parallelizable with:** all features (independent).

### F21 — Pre-commit hooks

- **Complexity:** S.
- **Files:** `.pre-commit-config.yaml` (new), `.git/hooks/pre-commit` (commit-msg hook).
- **Tests added:** none (infrastructure).
- **Benchmark added:** none.
- **Docs added:** `docs/development.md` (new — describes the dev workflow).
- **Verification gates:** `pre-commit run --all-files` passes.
- **Commit message:** `chore(quality): pre-commit hooks (clang-format, build, test)`
- **Parallelizable with:** all features (independent).

---

## Part 3 — Execution Sequence

### Phase 1 (parallel, days 1-2) — Security foundation
- F8 (scrypt auth) — **critical path**
- F11 (trace IDs) — independent
- F15 (blocklist) — independent

**Advance to Phase 2 when:** F8 verifies a stored hash; F11 trace IDs propagate; F15 blocklist rejects banned IPs.

### Phase 2 (parallel, days 2-3) — Auth hardening
- F8b (auth rate-limit) — depends on F8
- F12 (TLS per-listener, ALPN) — independent
- F13 (audit log) — independent
- F19 (MQTT server stub) — defer formally

**Advance to Phase 3 when:** F8b caps 10/sec, F12 distinct CTX, F13 JSON-lines, F19 stub compiled.

### Phase 3 (parallel, days 3-5) — Account + access control
- F14 (per-account quota) — independent
- F16 (per-subject ACL) — independent
- F17 (inter-node TLS) — **serialized against other route changes**

**Advance to Phase 4 when:** all 3 features merged + verified.

### Phase 4 — Recovery + Quality
- F18 (persistent subs) — single PR, depends on F5 (already shipped)
- F20 (coverage + fuzz) — independent
- F21 (pre-commit) — independent

**Advance to Phase 5 when:** F18 passes restart-recovery test; F20 coverage ≥80% on new code; F21 hooks pass.

### Phase 5 — Final docs + tag
- Update CHANGELOG.md, README.md
- Tag `v0.3.0`
- Push to `origin main`

### Phase 6 — Done
- HANDOFF to user with summary

---

## Part 4 — Verification Gates

### Per-phase
- **Phase 1 → 2:** `ctest -j1` shows 36 + 3 new tests passing. F8's round-trip test passes. F11's trace ID test passes. F15's blocklist test passes.
- **Phase 2 → 3:** `ctest -j1` shows 36 + 7 new tests passing. F8b's rate-limit test passes. F12's per-listener test passes. F13's audit JSON-lines test passes.
- **Phase 3 → 4:** `ctest -j1` shows 36 + 10 new tests passing. F14's quota test passes. F16's ACL test passes. F17's route TLS test passes.
- **Phase 4 → 5:** `ctest -j1` shows 36 + 13 new tests passing. F18's restart test passes. F20's coverage ≥80%. F21's hooks pass.
- **Phase 5 → 6:** CHANGELOG.md updated, README.md updated, tag v0.3.0 pushed.

### Per-feature
- New tests pass + existing 36 still pass.
- New docs/features/<name>.md exists + non-empty.
- Commit message follows Conventional Commits: `feat/fix/docs/test(scope): description`.
- No compiler warnings.
- No new TODOs/FIXMEs in code.
- No `as any` / `TODO` / `XXX` added.

### Final (push to remote)
- `git tag -a v0.3.0 -m "<message>"`
- `git push origin main v0.3.0`
- `git remote -v` shows `git@github.com:konyka/cmsgqueue.git` (verified earlier).
- `git log --oneline v0.2.0..v0.3.0` shows exactly 13 feature commits + 1 docs commit = 14 commits.

---

## Part 5 — Commit & Push Plan

### Branch strategy
- `main` is the only protected branch (existing pattern).
- Each feature is a branch `feat/<name>` (e.g., `feat/scrypt-auth`).
- One PR per feature. Merge to `main` only after green CI.

### Number of PRs
- 13 feature PRs + 1 docs/push PR = **14 PRs total**.

### "Tests pass" definition
A PR is mergeable when ALL of:
1. `ctest -j1` shows 36 + new tests passing.
2. No compiler warnings (under `CMQ_ENABLE_HARDENING=ON`).
3. New docs/features/<name>.md exists, non-empty, follows template.
4. Commit message follows Conventional Commits.
5. Self-review: at least 1 reviewer approval.

### Push trigger
```bash
git tag -a v0.3.0 -m "v0.3.0: see CHANGELOG.md"
git push origin main v0.3.0
```

### Remote config (verified)
- `git remote -v` shows `origin git@github.com:konyka/cmsgqueue.git` (fetch + push).
- SSH key verified earlier: `ssh -T git@github.com` returns success.

---

## Part 6 — Open Questions for the User

1. **F8 scrypt vs PBKDF2**: scrypt has memory-hardness advantage but slower on first connect. The bundle chose scrypt; confirm?
   - **Recommendation:** scrypt (memory-hard, NIST-recommended).
2. **F18 persistent subs storage**: separate stream in same filestore, or separate file?
   - **Recommendation:** separate stream in same filestore (simpler).
3. **F14 quota reject vs drop**: bundle decided reject-with-error. Confirm?
   - **Recommendation:** reject (operator-friendly).
4. **F19 MQTT server**: bundle says defer to v0.4.0. Confirm v0.3.0 ships only the stub?
   - **Recommendation:** yes, defer (XL effort, not realistic for v0.3.0).
5. **F20 coverage threshold**: 80% per-feature or 80% global?
   - **Recommendation:** per-feature (easier to maintain, no legacy debt).

---

## Part 7 — Verbatim Defensible Decisions

Re-listed with file:line citations:

1. **F8 password hashing**: OpenSSL scrypt via `EVP_PBE_scrypt` (no libsodium). Wire format: `$scrypt$N=2^14,r=8,p=1$<salt-b64>$<hash-b64>`. Files: `src/server/cmq_password.{h,c}` (new). Migrate plaintext transparently.
2. **F8b auth brute-force**: per-IP rate limit at 10/sec. Files: extend `src/server/cmq_rate_limit.c`.
3. **F11 trace IDs**: 16-byte UUID per connection. Files: `src/server/cmq_trace.{h,c}` (new). Propagate to all log entries.
4. **F12 TLS hardening**: per-listener `SSL_CTX`, ALPN. Files: `src/enterprise/cmq_tls.{h,c}` (split).
5. **F13 audit log**: JSON-lines, separate file. Files: `src/enterprise/cmq_audit.{h,c}` (new).
6. **F14 quota**: per-account msgs/sec, bytes/sec, connections. Files: `src/enterprise/cmq_quota.{h,c}` (new). Reject on exceed.
7. **F15 blocklist**: IP/CIDR blocklist. Files: `src/cluster/cmq_blocklist.{h,c}` (new). Accept-time check.
8. **F16 ACL**: per-account subject allow/deny. Files: `src/enterprise/cmq_acl.{h,c}` (new). Default deny if neither set.
9. **F17 inter-node TLS**: wrap route sockets with TLS. Files: `src/cluster/cmq_route_tls.{h,c}` (new). Per-route shared secret.
10. **F18 persistent subs**: WAL stream for sublist state. Files: `src/server/cmq_sublist_persist.{h,c}` (new). Replay only delivers to live subs; cursor per sub.
11. **F19 MQTT server-side**: deferred to v0.4.0. Files: `src/enterprise/cmq_mqtt_server.{h,c}` (new with stub returning `ENOSYS`).
12. **F20 coverage + fuzz**: libfuzzer harnesses for parser, mqtt, filestore. Files: `tests/fuzz/`. CI gate ≥80% per feature.
13. **F21 pre-commit hooks**: clang-format + build + ctest. Files: `.pre-commit-config.yaml`.

### Critical-path summary
- F8 → F8b → F11 → F12 (security wedge)
- F18 standalone (depends on F5 only)
- F14, F15, F16, F17, F20, F21 parallel

### Tag
- `v0.3.0` on `origin main` after all 14 PRs merged and full ctest green.
