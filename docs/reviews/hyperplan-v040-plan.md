# CMSGQueue v0.3.0 → v0.4.0 — Final Executable Plan

**Provenance.** 3-round hyperplan review of v0.3.0 (HEAD 5e02805, 45/45 tests, 22 docs). Source bundle: `docs/reviews/hyperplan-v040-bundle.md`. Lead-orchestrator synthesis of 2/5 returned adversarial agents (security/perf + TDD/docs) plus direct codebase verification for the remaining 3.
**Constraint envelope.** C11, NATS-like, performance-first, secure, reliable, TDD, atomic commits, push to `origin main` only after full CI matrix green.
**Bundle source-of-truth.** `docs/reviews/hyperplan-v040-bundle.md` (this plan refines, never contradicts).

---

## Part 1 — Plan Overview

### Goal
Ship v0.4.0 with **7 features** merged behind TDD-tested, benchmarked, documented PRs:
1. test_stress flake fix (S)
2. F12 TLS per-listener + ALPN + cert reload + mTLS flag (M)
3. F14/F15/F16 library wire-up into the server (S)
4. F17 inter-node TLS via `BIO_wrap` over the route socket (L)
5. F18 persistent subscription state via the F5 replay path (L)
6. F19 server-side MQTT 3.1.1 listener (XL — QoS 0 + CONNECT/PUBLISH/SUBSCRIBE/PING only; defer 5.0 + QoS 2 to v0.5.0)
7. Hardening pass: HSTS + audit rotation + graceful shutdown timeout (S)

Final tag `v0.4.0`, pushed to `origin main` after full CI matrix green and the perf regression gate is satisfied.

### Total scope

| Bucket | Count |
|---|---|
| Source files created | ~14 |
| Source files modified | ~7 |
| New tests | 7 (≥ 25 scenarios) + 1 new MQTT fuzz harness |
| New benches | 3 |
| PRs | 7 (one per feature) + 1 docs/push = **8 PRs** |
| Docs | 7 in `docs/features/` (replacing 2 stubs + 5 new/expanded) + 1 CHANGELOG + 1 ADR |
| Final tag | `v0.4.0` |
| Approx LOC added | ~3 200 impl + ~2 100 tests + ~900 docs (~6 200 total) |

### Critical path
```
F1 (test_stress flake)  ─┐
                          ├─→ F2 (F12 TLS) ─→ F3 (wire-up) ─→ F7 (hardening PR) ─→ tag v0.4.0 → push
F4 (F17 inter-node TLS)  ─┤
F5 (F18 persistent subs) ─┤
F6 (F19 MQTT listener)   ─┘
```
- **F1 (test_stress fix)** unblocks the flaky CI gate that today masks regressions.
- **F2 (F12)** is the largest independent change; ALPN + cert reload + mTLS flag.
- **F3 (F14/F15/F16 wire-up)** is small and unblocks F7's HSTS path (which needs `cmq_healthz` to know TLS is on).
- **F4 (F17)**, **F5 (F18)**, **F6 (F19)** are independent and can be developed in parallel branches.
- **F7 (hardening pass)** depends on F2 (HSTS needs `tls_configured()` to be true) and F3 (blocklist is consumed in the same `accept_cb`).

### Perf budget
- End-to-end msg/s: **≥ 32 000** (≤ 5 % regression from v0.3.0 baseline **33 784**).
- Avg latency: ≤ 35 µs (current 30 µs).
- TLS handshake (cached session): ≤ 10 µs.
- TLS handshake (cold): ≤ 5 ms.
- Inter-node encryption: ≤ 5 % CPU overhead vs plaintext.
- F18 replay: ≤ 10 ms for 100 K subscriptions.
- F19 MQTT (QoS 0): ≥ 26 K msg/s (≥ 80 % of cmq baseline).

### Risk register

| ID | Risk | Severity | Mitigation |
|---|---|---|---|
| R1 | F17 BIO_wrap breaks cluster handshake if not careful with non-blocking | High | Phase 2 dedicated test for cluster handshake + TLS; fuzz harness for BIO state machine |
| R2 | F18 replay reorders subscriptions; clients may receive in-flight publishes before sub is restored | Medium | Replay only delivers to currently-subscribed subjects (F5's existing semantics); client-side re-subscribe is documented |
| R3 | F19 MQTT listener is XL; the 5-day effort is tight | High | Ship QoS 0 + 3.1.1 + CONNECT/PUBLISH/SUBSCRIBE/PING only; defer 5.0 + QoS 2 to v0.5.0; keep stub fallback (`ENOSYS`) if integration fails |
| R4 | mTLS verify breaks the existing test_tls_stub_refused test | Medium | Update test_tls_stub_refused to test the new fail-closed path; existing test_enterprise tls tests need a small update |
| R5 | F14/F15/F16 wire-up races with publish hot path | Low | All three are O(1) checks; wire before `cmq_sublist_match` (already the credit path); no extra locks |
| R6 | HSTS without TLS leaks "Strict-Transport-Security" header in cleartext | Low | HSTS is only sent when `tls_configured()` is true (post-F12); cleartext paths omit the header |

---

## Part 2 — Feature Plans

### F1 — test_stress flake fix

- **Complexity:** S.
- **Files:** `tests/test_stress.c`.
- **Tests added:** modify the existing `test_stress.many_clients_single_thread` to use a `pthread_barrier_t` so all subscribers have subscribed before any publisher fires; add an explicit `nanosleep(50ms)` settle after subscribe; add a per-sub counter and wait-for-all-or-timeout logic; add `test_stress.flake_regression_10_runs` that runs the test 10 times.
- **Benchmark added:** none (this is a correctness fix).
- **Docs added:** none (test change; the doc is the test).
- **Verification gates:** `for i in 1 2 3 4 5 6 7 8 9 10; do ./tests/test_stress; done` passes 10/10.
- **Commit message:** `test(stress): fix many_clients flake with subscribe-publish barrier`
- **Risk notes:** none.
- **Parallelizable with:** all features.

### F2 — F12 TLS hardening

- **Complexity:** M.
- **Files:** `src/enterprise/cmq_tls.{h,c}`, `src/server/cmq_server.c` (use per-listener CTX), `tests/test_tls_per_listener.c`.
- **Tests added:**
  - `tls_per_listener.two_listeners_distinct_ctx` — verifies the two listeners have independent SSL_CTX (different certs / different cipher policies).
  - `tls_per_listener.alpn_negotiation` — connects with `"h2"` and `"http/1.1"`, asserts server selects accordingly.
  - `tls_per_listener.cert_reload_no_disconnect` — start a session, call `cmq_tls_config_reload`, new connections use the new cert, existing connections stay on the old.
  - `tls_per_listener.mtls_required` — server-side `SSL_VERIFY_PEER` + client cert; missing client cert rejected.
  - `tls_per_listener.mtls_disabled` — server-side `SSL_VERIFY_PEER` off; client without cert accepted (legacy mode).
- **Benchmark added:** `bench/bench_tls_handshake.c` — cold + warm handshake latency; ALPN overhead; mTLS overhead.
- **Docs added:** `docs/features/tls-hardening.md` (rewrites tls-openssl.md), `docs/adr/0010-per-listener-ssl-ctx.md`.
- **Verification gates:**
  - 5/5 new tests pass.
  - `readelf` on `cmsg_server` shows `BIND_NOW` and `PIE` (already in v0.3.0).
  - `cmake -DCMQ_ENABLE_TLS=ON` builds, runs `test_tls_per_listener`, returns 0.
  - Existing tests `test_enterprise tls.config_set_fields` and `tls.session_lifecycle` still pass (with small updates to assert new fields).
- **Commit message:** `feat(enterprise): F12 TLS per-listener SSL_CTX with ALPN + cert reload + mTLS`
- **Risk notes:** the existing `test_tls_stub_refused` test asserts `cmq_server_create` fails when TLS is enabled with no cert. With per-listener CTX, the fail-closed path stays the same (no cert → no listener → server fails). The test should still pass.
- **Parallelizable with:** F4, F5, F6, F1.

### F3 — F14/F15/F16 wire-up

- **Complexity:** S.
- **Files:** `src/server/cmq_server.{h,c}` (add `blocklist`, `quota`, `acl` server fields and their initializers/destructors), `src/server/cmq_config.c` (add config keys `blocklist_file`, `max_msgs_per_sec_per_account`, `max_bytes_per_sec_per_account`, `max_connections_per_account`, `acl_allow`, `acl_deny`), `tests/test_blocklist.{c}`, `tests/test_quota.{c}`, `tests/test_acl.{c}` (extend with integration tests that wire the libraries into a fake server path).
- **Tests added:**
  - `quota.integration_rejects_on_exceed` — open server with quota cap, send many msgs, last N rejected.
  - `blocklist.integration_rejects_banned_ip` — open server with blocklist file, banned IP rejected pre-handshake.
  - `acl.integration_deny_rejects_publish` — open server with deny-list, matching subject rejected.
- **Benchmark added:** none (off-hot-path; same as F14/F15/F16 library tests).
- **Docs added:** none (the existing F14/F15/F16 docs are extended with a "server-side wiring" section).
- **Verification gates:**
  - 3 new integration tests pass.
  - 45/45 + 3 new = 48/48 (existing tests still pass).
  - Per-IP rate limit (F10) and auth brute-force (F8b) continue to work (no overlap).
- **Commit message:** `feat(server): wire F14/F15/F16 (quota/blocklist/ACL) into server`
- **Risk notes:** none significant. The order of checks in `handle_publish` is: account_can_export (existing) → quota_check (new) → acl_check (new) → sublist_match.
- **Parallelizable with:** F2, F4, F5, F6, F1.

### F4 — F17 inter-node TLS

- **Complexity:** L.
- **Files:** `src/cluster/cmq_route_tls.{h,c}` (new), `src/cluster/cmq_route.c` (wrap socket on connect/accept), `tests/test_route_tls.c`.
- **Tests added:**
  - `route_tls.two_routes_negotiate_tls13` — two route peers establish TLS 1.3.
  - `route_tls.cleartext_disabled_when_configured` — config flag `route_require_tls = 1` rejects cleartext.
  - `route_tls.mtls_two_routes` — both sides have certs; verify chain.
  - `route_tls.handshake_failure_does_not_crash` — bad cert rejected, no crash.
  - `route_tls.throughput_overhead` — bench-style assertion that AES-GCM is < 5 % CPU overhead (skipped under CI without `-DCMAKE_BUILD_TYPE=Release`).
- **Benchmark added:** `bench/bench_route_tls.c` — AES-GCM-NI vs plaintext msg/s.
- **Docs added:** `docs/features/route-tls.md`, `docs/adr/0011-route-tls-shared-secret.md`.
- **Verification gates:**
  - 5/5 new tests pass.
  - Existing `test_cluster` still passes (cluster without TLS unchanged).
- **Commit message:** `feat(cluster): F17 TLS-wrapped route connections with shared secret`
- **Risk notes:** route connect is in `cmq_route_io_loop`. The TLS handshake uses non-blocking BIO; the existing `EAGAIN` retry path handles partial reads/writes. New failure modes: `SSL_ERROR_WANT_READ/WRITE` mapped to `EAGAIN` so the existing event loop drives the handshake.
- **Parallelizable with:** F2, F3, F5, F6, F1.

### F5 — F18 persistent subscription state

- **Complexity:** L.
- **Files:** `src/server/cmq_sublist_persist.{h,c}` (fill in the stub), `src/server/cmq_sublist.{h,c}` (mirror adds/removes to the WAL), `src/server/cmq_server.c` (call `cmq_sublist_persist_load` after the F5 replay loop), `tests/test_sublist_persist.c` (replace the stub test).
- **Tests added:**
  - `sublist_persist.subscribe_persists` — subscribe, restart server, verify in-memory sublist has the entry.
  - `sublist_persist.unsubscribe_removes` — subscribe, unsubscribe, restart, verify removed.
  - `sublist_persist.replay_only_live_subs` — publish, restart, current subs receive; old subs do not.
  - `sublist_persist.snapshot_compact` — periodic snapshot reduces replay time.
- **Benchmark added:** `bench/bench_sublist_persist.c` — 100K subs startup time.
- **Docs added:** `docs/features/sublist-persist.md` (rewrites the stub doc), `docs/adr/0012-persistent-subs-wal.md`.
- **Verification gates:**
  - 4/4 new tests pass (replacing the 2 stub tests).
  - Restart-recovery time < 10 ms for 100 K subscriptions.
- **Commit message:** `feat(server): F18 persistent subscription state (WAL mirror + replay)`
- **Risk notes:** the in-memory `cmq_sublist` has its own locking; the WAL mirror must hold the same lock to avoid races between sub/unsub and replay. Design: `cmq_sublist_persist_record_sub` acquires `sublist->lock`, writes to WAL, then calls `cmq_sublist_add`.
- **Parallelizable with:** F2, F3, F4, F6, F1.

### F6 — F19 server-side MQTT 3.1.1 listener

- **Complexity:** XL.
- **Files:** `src/enterprise/cmq_mqtt_server.{h,c}` (replace stub with full protocol parser), `src/server/cmq_server.c` (start the listener thread on `mqtt_port`), `tests/test_mqtt_listen.c`.
- **Tests added:**
  - `mqtt_listen.connect_publish_subscribe` — end-to-end: CONNECT, PUBLISH, SUBSCRIBE, message received.
  - `mqtt_listen.qos0_at_most_once` — QoS 0 PUB.
  - `mqtt_listen.qos1_at_least_once` — QoS 1 PUB + PUBACK.
  - `mqtt_listen.pingreq_pingresp` — keepalive.
  - `mqtt_listen.disconnect_clean` — graceful disconnect.
  - `mqtt_listen.wildcard_subscription` — `foo/+` matches `foo/bar`.
- **Benchmark added:** `bench/bench_mqtt_listen.c` — QoS 0/1 throughput.
- **Docs added:** `docs/features/mqtt-server-listener.md` (rewrites mqtt-server-stub.md), `docs/adr/0013-mqtt-listener.md`.
- **Verification gates:**
  - 6/6 new tests pass.
  - `mosquitto_pub -h 127.0.0.1 -p 1883 -t foo -m hello` end-to-end test passes.
  - Existing tests still pass (45 + 6 = 51/51).
- **Commit message:** `feat(enterprise): F19 server-side MQTT 3.1.1 listener (CONNECT/PUBLISH/SUBSCRIBE/PING/DISCONNECT)`
- **Risk notes:** the listener runs in its own thread (separate accept loop). State machine per connection. Uses the existing `cmq_sublist` for routing. Falls back to stub (`-ENOSYS`) if integration fails — keeps `v0.4.0` shippable even if F19 lands incomplete.
- **Parallelizable with:** F2, F3, F4, F5, F1.

### F7 — Hardening pass

- **Complexity:** S.
- **Files:** `src/server/cmq_server.c` (HSTS header on /healthz/readyz/metrics when TLS configured; graceful shutdown timeout), `src/enterprise/cmq_audit.c` (size-cap rotation), `tests/test_health_metrics.c` (extend), `tests/test_audit.c` (extend), `tests/test_server_drain.c` (new).
- **Tests added:**
  - `health_metrics.hsts_sent_when_tls` — TLS on, response includes `Strict-Transport-Security`.
  - `health_metrics.no_hsts_when_cleartext` — TLS off, header absent.
  - `audit.rotation_at_100mb` — write 100 MiB+1, file rolled to `.1`.
  - `server_drain.timeout_clean` — drain with 100 ms timeout, half-open connections closed.
- **Benchmark added:** none (audit rotation is off-hot-path).
- **Docs added:** none (the audit doc and health-metrics doc are extended in-place).
- **Verification gates:**
  - 4 new tests pass.
  - 51 + 4 = 55/55 (target).
- **Commit message:** `chore(hardening): HSTS + audit rotation + graceful shutdown timeout`
- **Risk notes:** graceful shutdown timeout can drop in-flight publishes; the test verifies the documented behavior. Audit rotation requires `logrotate` integration — we ship the size-cap path; system-level logrotate is a deployment concern.
- **Parallelizable with:** F2, F3, F4, F5, F6, F1 (but is sequenced last in Phase 4).

---

## Part 3 — Execution Sequence

### Phase 1 (parallel, day 1)
- F1 (test_stress flake fix) — unblocks the test gate.
- F2 (F12 TLS per-listener) — foundation for F7.
- F4 (F17 inter-node TLS).
- F5 (F18 persistent subs).

**Advance to Phase 2 when:** F1's `flake_regression_10_runs` passes 10/10; F2's `tls_per_listener` tests pass.

### Phase 2 (parallel, day 2–3)
- F3 (F14/F15/F16 wire-up).
- F6 (F19 MQTT listener).

**Advance to Phase 3 when:** F3's 3 integration tests pass; F6's 6 listener tests pass; existing `test_enterprise tls.config_set_fields` updated for new fields.

### Phase 3 (parallel, day 3–5)
- Continue F6 (MQTT protocol state machine).
- Run perf benchmarks for F2, F4, F5.

**Advance to Phase 4 when:** all 7 features merged; full ctest green; perf baseline within 5 %.

### Phase 4 (sequential)
- F7 (hardening pass: HSTS + audit rotation + graceful shutdown).
- Update CHANGELOG, README.
- Tag `v0.4.0`.
- Push to `origin main`.

### Phase 5 — Done
- Handoff to user with summary.

---

## Part 4 — Verification Gates

### Per-phase
- **Phase 1 → 2:** `test_stress` passes 10/10 runs. F12's per-listener SSL_CTX tests pass.
- **Phase 2 → 3:** F14/F15/F16 integration tests pass. F19's MQTT listener tests pass.
- **Phase 3 → 4:** All 7 features merged. Full `ctest -j1` green. End-to-end perf ≥ 32 000 msg/s.
- **Phase 4 → done:** F7 hardening tests pass. `v0.4.0` tag created. `git push origin main v0.4.0` succeeds.

### Per-feature
- New tests pass + existing 45 tests still pass (45 → 51 after F1+F2+F3+F4+F5+F6+F7).
- New docs/features/<name>.md exists, non-empty, follows template.
- Commit message follows Conventional Commits: `feat/fix/test/chore(scope): description`.
- No compiler warnings under `CMQ_ENABLE_HARDENING=ON`.
- No new TODOs/FIXMEs.
- No `as any` / `unimplemented` markers added.

### Final (push to remote)
- `git tag -a v0.4.0 -m "<message>"`
- `git push origin main v0.4.0`
- `git log --oneline v0.3.0..v0.4.0` shows 8 commits (7 feature + 1 docs/push).
- All `tests/` pass.
- End-to-end benchmark ≥ 32 000 msg/s.

---

## Part 5 — Commit & Push Plan

### Branch strategy
- `main` is the only protected branch (existing pattern).
- Each feature is a branch `feat/<name>` (e.g., `feat/tls-per-listener`).
- One PR per feature. Merge to `main` only after green CI.

### Number of PRs
- 7 feature PRs + 1 docs/push PR = **8 PRs total**.

### "Tests pass" definition
A PR is mergeable when ALL of:
1. `ctest -j1` shows 45 + new tests passing.
2. No compiler warnings (under `CMQ_ENABLE_HARDENING=ON`).
3. New docs/features/<name>.md exists, non-empty, follows template.
4. Commit message follows Conventional Commits.
5. Self-review: at least 1 reviewer approval.

### Push trigger
```bash
git tag -a v0.4.0 -m "v0.4.0: see CHANGELOG.md"
git push origin main v0.4.0
```

### Remote config (verified)
- `git remote -v` shows `origin git@github.com:konyka/cmsgqueue.git` (fetch + push).
- SSH key verified: `ssh -T git@github.com` returns success.

---

## Part 6 — Open Questions for the User

1. **F12 mTLS scope.** Bundle says ship cert-reload + ALPN + mTLS flag. Confirm? (Recommended: ship together; mTLS verify is small once `SSL_VERIFY_PEER` is wired.)
2. **F19 MQTT scope.** Bundle says QoS 0/1 + 3.1.1 only; defer 5.0 + QoS 2. Confirm? (Recommended: yes; XL effort, focus on the common case.)
3. **F14/F15/F16 wire-up — fail-closed or fail-open?** Bundle says reject (operator-friendly). Confirm? (Recommended: reject.)
4. **F18 replay semantics.** Bundle says replay only delivers to currently-subscribed subjects. Confirm? (Recommended: yes; matches F5's existing semantics.)
5. **Hardening pass scope.** Bundle says HSTS + audit rotation + graceful shutdown timeout. Confirm? (Recommended: yes; all small and high-impact.)

---

## Part 7 — Verbatim Defensible Decisions

Re-listed with file:line citations:

1. **F1 test_stress flake fix** — `tests/test_stress.c:241` `ASSERT(total_received >= total_msgs)` fails ~20% of runs. Add `pthread_barrier_t` so all subscribers are ready before any publisher fires. Files: `tests/test_stress.c`.
2. **F12 TLS per-listener** — `src/enterprise/cmq_tls.c:96` builds ONE SSL_CTX. Replace with per-listener array. `cfg->ca` at line 134 wires `SSL_VERIFY_PEER`. Files: `src/enterprise/cmq_tls.{h,c}`, `src/server/cmq_server.c`.
3. **F14/F15/F16 wire-up** — `src/server/cmq_server.c` calls `cmq_blocklist_check`, `cmq_quota_check_publish`, `cmq_acl_check` at `handle_publish` before sublist match. Files: `src/server/cmq_server.{h,c}`, `src/server/cmq_config.c`.
4. **F17 inter-node TLS** — `src/cluster/cmq_route.c` is plaintext. `BIO_wrap` on route socket. Files: `src/cluster/cmq_route_tls.{h,c}` (new), `src/cluster/cmq_route.c`.
5. **F18 persistent subs** — `src/server/cmq_sublist_persist.c` returns NULL/-1. Fill in WAL mirror. Files: `src/server/cmq_sublist_persist.{h,c}`, `src/server/cmq_sublist.{h,c}`, `src/server/cmq_server.c`.
6. **F19 MQTT listener** — `src/enterprise/cmq_mqtt_server.c` returns `-ENOSYS`. Full 3.1.1 protocol. Files: `src/enterprise/cmq_mqtt_server.{h,c}`, `src/server/cmq_server.c`.
7. **Hardening pass** — `src/server/cmq_server.c` sends DISCONNECT; `cmq_audit.c` opens with "a". Add HSTS, audit rotation, drain timeout. Files: `src/server/cmq_server.c`, `src/enterprise/cmq_audit.c`.

### Critical-path summary
- F1 (test_stress fix) → F2 (F12 TLS) → F3 (wire-up) → F7 (hardening) → tag.
- F4 (F17), F5 (F18), F6 (F19) parallel.

### Tag
- `v0.4.0` on `origin main` after 8 commits merged and full ctest + perf baseline matches.
