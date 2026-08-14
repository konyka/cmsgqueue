# CMSGQueue v0.4.0 → v0.5.0 Bundle

**Provenance.** v0.4.0 (HEAD 3a279a0, 51/51 tests, 22 docs). Direct codebase audit by lead orchestrator.

---

## 1. Confirmed Gaps (verified by file:line)

| ID | Item | Status | Evidence |
|---|---|---|---|
| 1 | F18 wire-up | PARTIAL | `cmq_sublist_persist_record_sub` not called in `src/server/cmq_server.c` `handle_subscribe`. Library exists; needs call site. |
| 2 | F14 enforcement | PARTIAL | `cmq_quota_check_publish` exists in `src/enterprise/cmq_quota.h` but not called in `cmq_server.c` `handle_publish`. |
| 3 | F15 enforcement | PARTIAL | `cmq_blocklist_check` not called in `accept_cb` in `src/server/cmq_server.c`. |
| 4 | F16 enforcement | PARTIAL | `cmq_acl_check` not called in `handle_publish`. |
| 5 | F17 BIO-wrap | OPEN | `cmq_route.c` uses plain `read`/`write` syscalls. `cmq_route_tls` library provides config but no socket wrapper. |
| 6 | F19 MQTT 5.0 + QoS 2 | OPEN | `cmq_mqtt_server_listen` returns -ENOSYS. |
| 7 | WAL replay parallelization | OPEN | F5 replay is O(N) single-threaded in `cmq_server_create`. |
| 8 | mTLS client cert chain | OPEN | F12 wires `SSL_VERIFY_PEER` but no custom CRL/OCSP. |

## 2. New Features Identified (post-v0.4.0)

| ID | Item | Effort | Priority |
|---|---|---|---|
| N1 | Per-subject rate limit (token bucket per subject) | M | Med |
| N2 | Hot config reload (SIGHUP) | S | Med |
| N3 | Audit log rotation test | XS | High |
| N4 | OTel tracing exporter | XL | Defer to v0.6.0 |
| N5 | HTTP/2 listener (alongside ALPN) | L | Defer to v0.6.0 |
| N6 | Persistent client connection resume | XL | Defer (client SDK feature) |

## 3. Defensible Decisions

### 3.1 F18 wire-up
Call `cmq_sublist_persist_record_sub` from `handle_subscribe` and `cmq_sublist_persist_record_unsub` from `handle_unsubscribe`. The library is already in `src/server/cmq_sublist_persist.c`. The `cmq_server_t` struct needs a `cmq_sublist_persist_t *persist` field. The F5 replay path on startup can be extended to also call `cmq_sublist_persist_load` and re-establish subscriptions.

### 3.2 F14/F15/F16 enforcement
**Enforce in `handle_publish` and `accept_cb`:**
- `handle_publish`: after the F5 persistence hook and before the sublist match, call `cmq_acl_check(srv->acl, account, subject)`, then `cmq_quota_check_publish(srv->quota, account, msg_len)`. Reject on failure with `cmq_send_error`.
- `accept_cb`: after the F8b auth rate-limit check, call `cmq_blocklist_check(srv->blocklist, peer_ip)`. Close on match.

### 3.3 F17 BIO-wrap
Replace the `read`/`write` syscalls in `cmq_route.c` with `SSL_read`/`SSL_write` when the route is configured with `route_require_tls`. Use a `cmq_route_tls_session_t` (per-route SSL*) attached to the route peer struct. The session is created on `cmq_route_open` and torn down on `cmq_route_close`.

### 3.4 F19 MQTT 5.0 + QoS 2
The full protocol state machine: extend `cmq_mqtt_server_listen` from -ENOSYS to a real implementation. The existing `cmq_mqtt` library has encode for CONNECT/PUBLISH/etc.; the server needs decode + state tracking per connection. Estimate: XL (4-6 days for one engineer). Ship 3.1.1 + QoS 0/1 only in v0.5.0; defer 5.0 properties and QoS 2 to v0.5.1.

### 3.5 WAL replay parallelization
Partition records by subject hash, replay each partition in a worker thread. Use the existing F5 recovery's `for (uint64_t seq = 1; seq <= last; seq++)` loop, but chunk it. The benefit is bounded by single-threaded sublist rebuild which dominates.

### 3.6 mTLS client cert chain
Add `SSL_CTX_set_client_CA_list` from the `ca` field. Custom CRL is a separate feature; the default OpenSSL chain validation is sufficient for v0.5.0.

### 3.7 N1 Per-subject rate limit
Token bucket per subject. New module `cmq_subject_rl.c`. Config: `max_msgs_per_sec_per_subject`. Enforce in `handle_publish` after the F14 quota check.

### 3.8 N2 Hot config reload
SIGHUP handler that re-reads the config file. cmq_config_load already exists. Add `cmq_server_reload(srv, path)` that re-reads and re-applies dynamic config (blocklist file, audit path). TLS cert reload (F12) already works.

## 4. TDD Order

| # | Feature | Effort | Blocked by |
|---|---|---|---|
| 1 | F18 wire-up | S | nothing |
| 2 | F14/F15/F16 enforcement | M | nothing |
| 3 | N3 audit rotation test | XS | nothing |
| 4 | F17 BIO-wrap | L | nothing |
| 5 | F19 MQTT 5.0 + QoS 2 (deferred 5.0 specifics) | XL | nothing |
| 6 | N1 per-subject rate limit | M | nothing |
| 7 | F18 server-side wiring + replay integration | M | #1 |
| 8 | N2 hot config reload | S | nothing |
| 9 | mTLS client cert chain | S | nothing |
| 10 | WAL replay parallelization | L | nothing |

**Critical path:** #1 → #2 → #7 (F18 server-side wiring). The wire-up is small and unblocks F18's true value (subs survive restart).

## 5. Performance Budget

- End-to-end msg/s: ≥33,000 (≤3% regression from v0.4.0 baseline 33,704).
- TLS handshake: ≤5 ms (unchanged from v0.4.0).
- F17 inter-node TLS: ≤5% CPU overhead.
- F18 startup replay: ≤10 ms for 100K subs.

## 6. Stop Conditions for Plan Agent

- Produce a sequenced, parallelized workflow with verification gates.
- Per-feature: tests, docs, commit.
- Tag `v0.5.0` at the end; push to `origin`.
