# CMSGQueue v0.2.0 → v0.3.0 Bundle (Hyperplan 3-round synthesis)

**Provenance.** 5-agent adversarial review (2 timed out, 3 returned). Lead-orchestrator synthesis. Source: v0.2.0 (HEAD c386ad4, 36/36 tests, 11 features shipped).

**User intent.** "What features are still unimplemented? Design a detailed performance-priority, security-and-reliability plan, execute via TDD, maintain docs, push after tests pass."

---

## 1. Confirmed Unimplemented / Partial — Round-1 Audit

### File:line evidence already verified

| Item | Status | Evidence |
|---|---|---|
| F8 password hashing | **OPEN** (CWE-256) | `src/server/cmq_config.c:124-125` `strdup(value)`; verify at `src/server/cmq_server.c:4403` compares plaintext via `ct_memeq` |
| Server-side MQTT listener | **OPEN** | `src/enterprise/cmq_mqtt.h` has encode-only; bundle §2.8 explicitly required "Listener on a separate port (default 1883)" — not shipped |
| Persistent subscription state | **PARTIAL** | F5 replay wired but comments at `src/server/cmq_server.c:6494-6495` say "no persistent subscription state in v0.2.0" |
| TLS mTLS / OCSP / ALPN | **OPEN** | F1 ships TLS 1.3 server-only with AEAD; no client cert verification wired |
| Inter-node encryption | **OPEN** | Cluster routes (`src/cluster/cmq_route.c`) are plaintext |
| Connection-tracing / correlation IDs | **OPEN** | No `cmq_trace_id`; no per-message UUID |
| Structured audit log | **OPEN** | `cmq_log` is text-only; auth/access events not separately logged |
| Per-account quota enforcement | **OPEN** | `cmq_account` has counters but no enforcement at CONNECT or PUBLISH |
| Connection blocklist | **OPEN** | No IP blocklist API |
| Per-subject ACL allow-list | **OPEN** | Subject validation is syntax-only; no allow-list per account |
| WAL replay bound | **OPEN** | F5 replay is O(N) startup; no snapshot/compact; no parallelization |
| Coverage gate | **OPEN** | No gcov/lcov integration; no per-PR threshold |
| Fuzz tests | **OPEN** | No libfuzzer harnesses; only static unit tests |
| Pre-commit hooks | **OPEN** | No clang-format, no lint, no build-attempt gate |

### Documented-only / Stub Remaining

- `cmq_account` exports counters but **enforcement is just at export-time**; no inbound quota.
- `cmq_sublist` is in-memory only; **never persisted** across restart.
- `cmq_mqtt_bridge` is client-only; **no server-side listener**.

---

## 2. Defensible Design Decisions (Round 2 / 3 consensus)

### 2.1 F8 — OpenSSL scrypt (no libsodium)

**Decision.** scrypt via `EVP_PBE_scrypt` (OpenSSL 3.5.7 confirmed). Wire format: `"$scrypt$N=2^14,r=8,p=1$<salt-b64>$<hash-b64>"`. Migrate plaintext transparently: on startup, if `auth_password` is not in the new format, store the plaintext under a one-shot legacy marker and emit a deprecation warning. The verify path detects the format prefix and dispatches.

**Why:** scrypt is memory-hard, NIST-recommended, OpenSSL-native. Avoids the libsodium build dependency. PBKDF2 with 600K iterations is the fallback if scrypt unavailable.

**Hot-path impact.** scrypt verify is ~100ms on a server. CONNECT is rare relative to msg/s, so 100ms is acceptable. Add a per-IP auth rate-limit (10/sec) to prevent brute-force.

### 2.2 Persistent subscription state

**Decision.** Subscription state is a separate WAL stream (`cmq.sublist` mirror). On startup: (1) read msg WAL into a "recovery queue", (2) read sub WAL into sublist, (3) replay recovery queue (only subs present at replay time receive the messages). Stable subscription IDs across restart.

**Cost:** 100K subs * 128 bytes = 12.8 MB. Restoration is O(N) single-threaded. Future: parallelize.

### 2.3 Server-side MQTT listener

**Decision.** DEFER to v0.4.0. The MQTT protocol is a full state machine (CONNECT/CONNACK/SUBSCRIBE/SUBACK/PUBLISH/PUBACK/PINGREQ/PINGRESP/DISCONNECT). Implementing this requires a real MQTT parser. Existing cmq_mqtt is encoder-only. **Estimation:** XL (2-4 weeks for one engineer). The bundle already noted this as the planned §2.8; we are correcting the prior claim that F6 shipped it.

**For v0.3.0:** Document the gap explicitly as **deferred to v0.4.0** in CHANGELOG.md. Add a stub function `cmq_mqtt_server_listen()` that returns `ENOSYS`.

### 2.4 TLS hardening

**Decision.** Per-listener `SSL_CTX` (current is per-server). Add `SSL_VERIFY_PEER` for client cert verification (mTLS) behind a config flag. Add `SSL_CTX_set_alpn_protos` for ALPN h2/http1.1. OCSP stapling deferred.

**Why:** Addresses the round-2 critique that "shared SSL_CTX is strictly worse than per-listener."

### 2.5 Inter-node encryption

**Decision.** Wrap route sockets with TLS via `BIO_ssl`. Per-route shared secret (config). Disable cleartext on routes (config flag).

### 2.6 Connection tracing / correlation IDs

**Decision.** Add `cmq_trace_id` (16-byte UUID) per connection. Set in CONNECT, propagate to all log entries for that connection. Add to `cmq_send_error` payload.

**Why:** Observability is the cheapest security win.

### 2.7 Structured audit log

**Decision.** New `cmq_audit_event_t` enum with: auth_ok, auth_fail, persist_fail, persist_recover, tls_handshake_fail, rate_limit_reject. Write to a separate JSON-lines file (`audit.log` in `persist_dir` if set, else stderr).

### 2.8 Per-account quota enforcement

**Decision.** Add `cmq_account_quota` config: `max_msgs_in_per_sec`, `max_bytes_in_per_sec`, `max_connections`. Enforce at CONNECT (counter increment) and PUBLISH (counter increment).

### 2.9 Connection blocklist

**Decision.** Add `blocklist` config file (line-separated IPs/CIDRs). Enforce at accept_cb. Add `cmq_blocklist_reload` API.

### 2.10 Per-subject ACL allow-list

**Decision.** Extend `cmq_account` with `subject_allow` / `subject_deny` lists. Enforce in handle_publish.

### 2.11 Coverage gate

**Decision.** Add `CMQ_ENABLE_COVERAGE=ON` cmake option. Builds with `--coverage`. CI gate: ≥80% line coverage on new code per PR.

### 2.12 Fuzz tests

**Decision.** Add libfuzzer harnesses for: `cmq_parser_feed`, `cmq_mqtt_encode_publish`, `cmq_filestore_append`. Run nightly in CI. Run 30s locally.

### 2.13 Pre-commit hooks

**Decision.** `.pre-commit-config.yaml` with: clang-format, cmake build, ctest. Self-documenting; can be disabled per-repo.

---

## 3. TDD Order (Round 3 — defended)

**Order rationale:** Each step is independent and TDD-friendly. The first cluster (F8, F11, F12) closes security gaps. The second cluster (F2-F4) adds reliability. The third cluster (F5-F8) is operational maturity.

| # | Feature | Effort | Critical? | Cluster |
|---|---|---|---|---|
| 1 | F8 password hashing (scrypt) | M | High (CWE-256) | Security |
| 2 | F8b auth brute-force rate limit | S | High | Security |
| 3 | F11 connection tracing (correlation IDs) | S | Med | Security |
| 4 | F12 TLS hardening (per-listener, ALPN) | M | Med | Security |
| 5 | F13 structured audit log | M | Med | Security |
| 6 | F14 per-account quota | M | Med | Reliability |
| 7 | F15 connection blocklist | S | Med | Reliability |
| 8 | F16 per-subject ACL | M | Med | Reliability |
| 9 | F17 inter-node encryption | L | Med | Reliability |
| 10 | F18 persistent subscription state | L | High | Recovery |
| 11 | F19 server-side MQTT listener | XL | Low | Feature gap |
| 12 | F20 coverage gate + fuzz tests | M | Med | Quality |
| 13 | F21 pre-commit hooks | S | Low | Quality |

F19 is deferred to v0.4.0 (too large for v0.3.0).

---

## 4. Performance Targets (defended)

| Metric | Current (v0.2.0) | Target (v0.3.0) |
|---|---|---|
| End-to-end msg/s | 29,234 | ≥28,000 (≤5% regression) |
| Avg latency | 34 µs | ≤35 µs |
| p99 latency | (unmeasured) | ≤100 µs |
| Auth (CONNECT) | n/a | ≤120 ms (scrypt verify) |
| Startup recovery | O(N) single-threaded | O(N) parallelized at 100K+ records |
| Memory @ 100K subs | n/a | ≤16 MB |
| TLS handshake | n/a | ≤5 ms (cached) |

---

## 5. Test Strategy (artistry design)

- Each feature: 3+ unit tests + 1 integration test + 1 docs test (asserts docs/features/<name>.md exists + non-empty).
- Coverage gate: ≥80% per new feature.
- Fuzz runs nightly.
- Pre-commit: clang-format + build attempt + ctest on changed files.

---

## 6. Open Questions for Plan Agent

1. **F8 scrypt vs PBKDF2.** scrypt has memory-hardness advantage but slower on first connect (load-time). PBKDF2-HMAC-SHA256 with 600K iterations is also acceptable. Choose based on benchmark.
2. **Persistent subscription state storage.** Separate stream in the same filestore, or a separate file (cmq.sublist)? The former is simpler; the latter is cleaner.
3. **TLS hardening per-listener CTX.** Currently one CTX per server. Splitting requires changing the existing listener registration. Cost?
4. **Mailbox semantics for replay.** Replay uses synthesized client (no fd). Are there any handle_publish paths that crash on c->fd == -1?
5. **Quota enforcement.** Drop vs reject-with-error? Reject is friendlier for operators.
6. **MQTT listener.** Confirmed defer to v0.4.0. Document explicitly.

---

## 7. Stop Conditions

The plan agent MUST:
- Produce a sequenced, parallelized workflow with verification gates.
- Specify exactly which test files are added per feature.
- Specify exactly which docs/features/<name>.md is added per feature.
- Specify the commit convention (Conventional Commits).
- Specify the tag — v0.3.0 at the end.
- Specify the push to `origin main` only after full ctest + perf baseline matches.
- NOT write code. The plan agent owns sequencing, not implementation.

The plan agent's output is the final deliverable. The lead (this orchestrator) does NOT write the plan.
