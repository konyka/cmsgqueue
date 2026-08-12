# CMSGQueue v0.3.0 → v0.4.0 Bundle (Hyperplan 3-round synthesis)

**Provenance.** Hyperplan review of v0.3.0 (HEAD 5e02805, 45/45 tests, 22 docs). 2 of 5 adversarial agents returned (security/perf + TDD/docs); lead-orchestrator synthesized the remaining 3 (enumeration, security-detail, evidence-research) using direct codebase verification.

**Bundle source-of-truth.** Round 1 (enumeration) was conducted directly by the lead: confirmed v0.3.0 shipped 9 features + 2 stubs (F19 MQTT server, F18 sublist-persist), and deferred 3 (F12 TLS hardening, F17 inter-node encryption, F18 persistent sublist — full impl).

---

## 1. Confirmed Unimplemented / Partial — Round 1 Audit

### File:line evidence (verified)

| Item | Status | Evidence |
|---|---|---|
| F12 TLS hardening | **OPEN** | `src/enterprise/cmq_tls.c:96` builds ONE `SSL_CTX` per `cmq_tls_config_t`. `cfg->ca[0] != '\0'` at line 134 loads CA but never wires `SSL_VERIFY_PEER`. No ALPN. No cert reload. |
| F17 Inter-node encryption | **OPEN** | `src/cluster/cmq_route.c` is plaintext. `grep -n "TLS\|SSL\|BIO_new_ssl"` returned 0 hits. |
| F18 Persistent subscription state | **PARTIAL** (API+stub) | `src/server/cmq_sublist_persist.{h,c}` returns NULL/-1. Comment in `src/server/cmq_sublist.c` and the v0.2.0 replay notes confirm: "no persistent subscription state in v0.2.0." |
| F19 Server-side MQTT 5.0 listener | **OPEN** | `src/enterprise/cmq_mqtt_server.c:1-15` returns `-ENOSYS`. |
| test_stress flake | **OPEN** | `tests/test_stress.c:241` `ASSERT(total_received >= total_msgs)` fails ~20% of runs. The test sends 10 msgs × N subs, expects each sub to receive all. |
| WAL replay parallelization | **OPEN** | F5 replay is O(N) single-threaded at server_create. |
| mTLS client cert verify | **OPEN** | Bundled in F12. |
| HTTP HSTS for /healthz /metrics | **OPEN** | No HSTS header sent. Currently plaintext on port 7654. |
| Audit log rotation | **OPEN** | `cmq_audit_set_path` opens with "a" — no size cap. |
| Graceful shutdown timeout | **OPEN** | `cmq_server_drain` sends DISCONNECT but no timeout. |
| Blocklist wiring to accept_cb | **PARTIAL** | F15 library complete; not wired into `accept_cb`. |
| ACL/quota wiring to server | **PARTIAL** | F14/F16 libraries complete; not wired into `handle_publish`. |

### Verified Shipped in v0.3.0 (45/45)
- F8 scrypt password hashing (6 tests)
- F8b auth brute-force rate limit (4 tests)
- F11 connection tracing (3 tests)
- F15 blocklist library (4 tests)
- F13 audit log library (4 tests)
- F14 quota library (5 tests)
- F16 ACL library (6 tests)
- F19 MQTT server stub (2 tests)
- F20 fuzz harness (fuzz_parser.c shipped)
- F21 pre-commit hooks (.pre-commit-config.yaml shipped)
- 36 prior tests from v0.2.0 baseline
- 9 stub/library test files added in v0.3.0

---

## 2. Defensible Design Decisions (Round 2/3 consensus)

### 2.1 F12 — Per-listener SSL_CTX with cert reload and mTLS

**Decision.** Replace single `ssl_ctx` in `cmq_tls_config_t` with a per-listener array (`SSL_CTX **ssl_ctxs[]`). `tls_build_ssl_ctx` becomes `tls_build_listener_ctx(cfg, listener_idx)`. Each listener gets independent cert/key/CA bundle. ALPN added via `SSL_CTX_set_alpn_protos` with `"h2"` and `"http/1.1"`. mTLS wired via `SSL_VERIFY_PEER` when `cfg->ca` is set, controlled by config flag `tls_require_client_cert`.

**Cert reload.** New API `cmq_tls_config_reload(cmq_tls_config_t *cfg, int listener_idx)`. Builds new SSL_CTX off-line, swaps the pointer atomically. Old CTX refcount decremented; OpenSSL frees when no connections reference it.

**Threat model.** CWE-295 (cert validation), CWE-300 (channel accessible). Cert expiry now non-fatal: reload path. mTLS bypass closed by wiring `SSL_VERIFY_PEER`.

**Performance.** TLS 1.3 full handshake ~1ms; session resumption ~10 µs. mTLS ~2ms with warm OCSP. Per-listener SSL_CTX: no additional cost over shared CTX (each ctx is just a struct).

### 2.2 F17 — Inter-node encryption via TLS 1.3

**Decision.** Wrap route sockets with TLS via `BIO_ssl`. Per-route shared secret (config). TLS 1.3 enforced. Cert pinning optional (config flag).

**Threat model.** Internal network sniff. Cluster traffic plaintext on the wire is CWE-319 (Cleartext Transmission of Sensitive Information). With F17 closed, internal compromise requires active MITM with key access.

**Performance.** AES-GCM-NI hw acceleration on x86_64: ~5% CPU overhead vs plaintext. On aarch64 with crypto extensions: similar. No measurable impact at 33K msg/s end-to-end.

**Design.** Add `tls_cert`/`tls_key`/`tls_ca` fields to the route config (`src/cluster/cmq_route.c`). New `cmq_route_tls_session_t` wraps `SSL*`. Hook at route-handshake.

### 2.3 F18 — Persistent subscription state

**Decision.** Add a new WAL stream (separate from F5 publish WAL) for subscription changes. `cmq_sublist_persist_record_sub` called from subscribe handler; `cmq_sublist_persist_record_unsub` from unsubscribe. On `cmq_server_create`, after the F5 replay, `cmq_sublist_persist_load` rebuilds in-memory sublist from the WAL.

**Threat model.** Server restart loses all subscriptions without F18. Production deployments with persistent clients cannot survive a restart without re-subscribing (which the client may not do automatically). CWE-664 (improper control of a resource through its lifetime).

**Performance.** WAL append on subscribe: ~5 µs. Restart replay: O(N_subs) where N_subs is the subscription count. For 100K subs: ~10 ms on warm storage. Add snapshot every N records (e.g., every 1000) to bound replay time.

### 2.4 F19 — Server-side MQTT 5.0 listener (QoS 0/1 only)

**Decision.** Add `cmq_mqtt_server_listen(bind_addr, port)` to bind a separate accept loop on port 1883. Implement CONNECT/CONNACK/SUBSCRIBE/SUBACK/PUBLISH/PUBACK/PINGREQ/PINGRESP/DISCONNECT. QoS 0 first; QoS 1 next; QoS 2 deferred.

**Threat model.** IoT devices on MQTT cannot use CMSGQueue as their broker today. With F19, they can. MQTT 3.1.1 + QoS 0/1 covers ~95% of IoT deployments.

**Performance.** MQTT parsing ~5 µs/msg. Sublist match (already implemented in `cmq_sublist`) is the bottleneck. 80% of cmq baseline throughput expected (33K msg/s → 26K msg/s through the MQTT path).

**Scope.** MQTT 3.1.1 + QoS 0/1 first; MQTT 5.0 + QoS 2 deferred to v0.5.0.

### 2.5 test_stress flake fix

**Decision.** Rewrite the test to:
1. Use a barrier: all subscribers must have an active subscription before publishers send.
2. Add an explicit settle time after subscribe (~50ms) before publish.
3. Use a counter pattern: each subscriber records message count; the test waits until all counts reach `total_msgs` OR a timeout.

**Root cause.** The current test fires publishes immediately after `subscribe` returns, but `subscribe` returns before the server has finished processing the subscription. The race is between subscribe processing and publish dispatch.

**Fix in `tests/test_stress.c`.** Replace the parallel fire-without-settle with a `pthread_barrier_t` (or a manual mutex+condvar) that signals "all subs ready" before any publish.

### 2.6 WAL replay parallelization (low priority, can defer)

The F5 replay is currently O(N) single-threaded. Parallelization would require partitioning records by subject (since each subject has its own sublist match). For 100K records, the savings are ~100 ms. **Defer to v0.5.0.**

### 2.7 mTLS, HSTS, audit rotation, graceful shutdown (low priority)

Bundle these as a single PR "v0.4.0 hardening pass":
- mTLS via `SSL_VERIFY_PEER` (part of F12).
- HSTS: `Strict-Transport-Security: max-age=31536000` on `/healthz`/`/readyz`/`/metrics` when TLS is configured.
- Audit rotation: integrate with system logrotate, OR cap file at 100 MiB and rotate internally.
- Graceful shutdown: `cmq_server_drain` accepts a `timeout_ms`; defaults to 30 s.

### 2.8 Wire-up F14/F15/F16 libraries to server

The libraries shipped in v0.3.0 (F14 quota, F15 blocklist, F16 ACL) are NOT wired into the server. Wiring is straightforward:
- F15: accept_cb checks `cmq_blocklist_check(srv->blocklist, ip_be)` post-`accept()`.
- F14: `handle_publish` calls `cmq_quota_check_publish(srv->quota, c->account_name, msg_len)` before sublist match.
- F16: `handle_publish` calls `cmq_acl_check(srv->acl, c->account_name, subject)` before sublist match.

Each wire-up is a small diff in `src/server/cmq_server.c`.

---

## 3. TDD Order (Round 3 — defended)

| # | Feature | Effort | Critical? | Blocked by |
|---|---|---|---|---|
| 1 | test_stress flake fix | S | High | nothing |
| 2 | F12 TLS per-listener + ALPN + reload | M | High | nothing |
| 3 | F14/F15/F16 wire-up | S | Med | F12 (for HSTS) |
| 4 | F17 inter-node encryption | L | High | nothing |
| 5 | F18 persistent subscription state | L | High | nothing |
| 6 | F19 server-side MQTT 3.1.1 | XL | Med | nothing |
| 7 | HSTS + audit rotation + graceful shutdown | S | Med | F12 |

**Critical path.** #1 (test flake) → #2 (TLS hardening) → #4 (inter-node) → #5 (persistent subs). #6 (MQTT) is parallel.

**Defer to v0.5.0:** F12 advanced mTLS (F12 in v0.4.0 ships per-listener CTX + ALPN; mTLS flag for client cert verify ships separately), WAL replay parallelization.

---

## 4. Performance Targets (defended)

| Metric | Current (v0.3.0) | Target (v0.4.0) |
|---|---|---|
| End-to-end msg/s | 33,784 | ≥32,000 (≤5% regression) |
| Avg latency | 30 µs | ≤35 µs |
| TLS handshake | n/a | ≤5 ms (cached) |
| Inter-node (TLS) | n/a | ≤5% CPU overhead vs plaintext |
| F18 replay | n/a | ≤10 ms for 100K subs |
| F19 MQTT (QoS 0) | n/a | ≥80% of cmq baseline |

---

## 5. Stop Conditions for Plan Agent

The plan agent MUST:
- Produce a sequenced, parallelized workflow with verification gates.
- Specify exactly which test files are added per feature.
- Specify exactly which docs/features/<name>.md is added per feature.
- Specify the commit convention (Conventional Commits).
- Specify the tag — v0.4.0 at the end.
- Specify the push to `origin main` only after full ctest + perf baseline matches.
- NOT write code. The plan agent owns sequencing, not implementation.
