# Changelog

## [0.5.8] - 2026-08-18

### Fixed
- **P2 mqtt sub/qos2 disconnect clear** — g_mqtt_sub_topics + g_qos2
  now cleared on DISCONNECT (previously only on CONNECT). Each session
  starts fresh.

### Added
- **P1 mqtt bridge payload freelist** — `g_mqtt_bridge_freelist[64]`
  reuses freed buffers. Capped at 64 entries; excess freed to heap.
  Steady state: 0 malloc + 0 free per message.
- **P1 TLS reload UAF regression test (real)** —
  `tests/test_tls_reload_safe.c` smoke-tests `cmq_tls_set_ca` +
  destroy under ASAN. Catches UAF regressions.

### Performance
- Bench mean 33 169 msg/s (similar to v0.5.7's 33 723, small
  variance). The freelist reuses buffers in steady state.

### Documentation
- `docs/reviews/v0.5.8.enumeration.md` — 17-item catalog.
- `docs/reviews/v0.5.8.plan.md` — 4-phase WBS.
- `docs/benchmarks/v058final_{1..5}.txt` — 5-run baseline (mean 33 169 msg/s, p99 99 µs).

### Test count
- 73 tests (was 72 in v0.5.7; +1 for `test_tls_reload_safe`).
- All 73 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime (M)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK (M)
- cmq_atomic_u64 32-bit portability
- TLS session cache (real)

## [0.5.7] - 2026-08-18

### Fixed
- **P1 mqtt_bridge_shutdown wired** — `cmq_server_destroy` calls
  `cmq_mqtt_bridge_shutdown` so the relay thread is signaled dying
  and joined before `srv->sublist` is torn down. No in-flight
  bridge payloads are lost.
- **P1 TLS load() checks set_cipher_list return** — was previously
  silently ignored; an invalid cipher string now returns -1 and
  frees the new CTX.
- **P1 accept fd leak on shutdown** — `cmq_server_stop` closes all
  `listen_fds[i]` eagerly. No fd leak under any path.

### Added
- **P3 freelist growth cap** — per-worker `msg_freelist_count`
  capped at `CMQ_WORKER_MSG_FREELIST_MAX=64`. Excess entries freed.
- **P3 mqtt_thread logs graceful exit** — `cmq_log_info("mqtt_thread
  exit fd=%d")` on thread return.
- **P1 close-by-fd documentation test** — `tests/test_close_by_fd.c`
  documents the existing `cmq_idmap + conn_gen` invariant.

### Performance
- Bench mean 33 723 msg/s (up from 32 600 in v0.5.6). The freelist
  reuses 0 of 0 mallocs in steady state.

### Documentation
- `docs/reviews/v0.5.7.enumeration.md` — 15-item catalog.
- `docs/reviews/v0.5.7.plan.md` — 4-phase WBS.
- `docs/benchmarks/v057final_{1..5}.txt` — 5-run baseline (mean 33 723 msg/s, p99 99 µs).

### Test count
- 72 tests (was 71 in v0.5.6; +1 for `test_close_by_fd`).
- All 72 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop (L)
- Multi-listener runtime (M)
- WS permessage-deflate (L)
- 5.0 wildcard PUBACK (M)
- cmq_atomic_u64 32-bit portability
- mqtt bridge freelist
- TLS regression test (v0.5.7 added a no-op verification)

## [0.5.6] - 2026-08-18

### Fixed
- **P1 F19b real bridge wire-up** — `cmq_server_create` now calls
  `cmq_mqtt_register_sublist_insert(&cmq_sublist_insert, srv->sublist)`.
  Without this the relay was a no-op. v0.5.6 ships the wire-up so the
  F19b bridge actually inserts mqtt PUBLISH topics into the cmq
  sublist.
- **P1 MQTT 5.0 SUBSCRIBE properties skip** — `mqtt_v5_props_skip` is
  honored in SUBSCRIBE too. v0.5.4 only did PUBLISH.

### Added
- **P1 rwlock fairness** — `cmq_rwlock_init` passes
  `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` on Linux/glibc.
  Writers no longer starve readers.
- **P2 handle_publish freelist** — per-worker `cmq_worker_msg_t`
  freelist. Zero per-message malloc/free in steady state.
- **P2 rate-limit mutex sharded 16-way** — 16 shards replace 1 global
  mutex in `rate_limit_check`. 16x throughput under high concurrent
  client count.
- **P3 log spam clamp** — `cmq_tls_set_crl(NULL)` logs at most 1/sec.

### Documentation
- `docs/reviews/v0.5.6.enumeration.md` — 14-item gap catalog.
- `docs/reviews/v0.5.6.plan.md` — 4-phase WBS.
- `docs/benchmarks/v056final_{1..5}.txt` — 5-run baseline (mean 32 600 msg/s, p99 99 µs).

### Test count
- 71 tests (no test count change vs v0.5.5; v0.5.6 is mostly refactor).
- All 71 green; bench gate passes.

### Deferred to v0.6
- Multi-threaded accept loop
- WS permessage-deflate
- Multi-listener runtime (slots[1..3])
- TLS session_init cache
- 32-bit cmq_atomic_u64 portability
- cmq_atomic_u64 close-by-fd verification test
- 5.0 wildcard PUBACK match

## [0.5.5] - 2026-08-18

### Fixed
- **P1 async WAL max payload size cap** — `cmq_filestore_set_max_payload_size` + default 1 MiB cap. OOM guard on hostile clients.
- **P1 sublist_insert no slot leak on malloc failure** — malloc before head++/count++ so a permanent failure can't leak ring entries.
- **P3 TLS load() checks return values** — ALPN + load_verify_locations failures now free the new CTX and return -1.
- **P3 stat_messages_out live/replay split** — same pattern as v0.5.3 P2 for stat_messages_in.

### Added
- **P1 MQTT per-source-IP rate limit** — `cmq_mqtt_set_rate_limit(capacity, refill_per_sec)` token bucket. Default off.
- **P1 MQTT bridge cleanup smoke test** — `tests/test_mqtt_bridge_cleanup.c`.

### Documentation
- `docs/reviews/v0.5.5.enumeration.md` — 11-item gap catalog.
- `docs/reviews/v0.5.5.plan.md` — 4-phase WBS.
- `docs/benchmarks/v055final_{1..5}.txt` — 5-run baseline (mean 32 625 msg/s, p99 99 µs).

### Test count
- 71 tests (was 70 in v0.5.4; +1 for `test_mqtt_bridge_cleanup`).
- All 71 green; bench gate passes.

### Deferred to v0.6
- WS permessage-deflate
- rwlock fairness
- close-by-fd protection
- multi-threaded accept
- Redis wire protocol, JWT auth

## [0.5.4] - 2026-08-18

### Fixed
- **P1 cmq_tls_reload UAF** — `SSL_CTX_up_ref(new_ctx)` so an in-flight SSL* doesn't dangle after the reload frees the old CTX.

### Added
- **P1 F19b bridge sublist_insert** — relay thread now calls `cmq_sublist_insert` via a function pointer registered by `cmq_mqtt_register_sublist_insert`.
- **P1 cmq_tls_set_crl() actually loads into X509_STORE** (already v0.5.3; expanded in v0.5.4).
- **P1 multi-listener runtime** — `cmq_server_t.listen_fds[4]` replaces single `listen_fd`; `#define listen_fd listen_fds[0]` preserves source back-compat.
- **P1 MQTT 5.0 property skip on PUBLISH** — variable-byte Property Length scanned+skipped.
- **P3 MQTT listener default-off** — `cmq_mqtt_set_listener_enabled(1)` opt-in. Default doesn't bind 1883.
- **P3 cmq_tls_set_crl NULL log** — misconfig is visible (vs v0.5.3 silent).
- **P3 stat_async_enqueued counter** — complements the v0.5.3 async_blocked.
- **P4 reset QoS2 + sub_topics on CONNECT** — table reset prevents long-running leak.

### Documentation
- `docs/reviews/v0.5.4.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.4.plan.md` — 4-phase WBS.
- `docs/benchmarks/v054final_{1..5}.txt` — 5-run baseline transcripts (mean 31 146 msg/s, p99 99 µs).

### Test count
- 70 tests (was 68 in v0.5.3; +2 for `test_mqtt_bridge_insert` + `test_sublist_concurrent`).
- All 70 green; bench gate passes.

## [0.5.3] - 2026-08-18

### Fixed
- **P1 CRL X509_STORE integration** — `cmq_tls_set_crl()` actually loads the PEM CRL into the SSL_CTX's X509_STORE now. v0.5.2 stored the path but never consulted the CRL. Revoked client certs are now rejected.
- **P2 STATS replay counter accuracy** — `stat_messages_replayed` only ticks when `handle_publish` actually runs (snapshots `stat_publishes_rejected` before/after).

### Added
- **P2 cmq_rch leak/UAF test** — 4 tests covering double-release, multi-acquire, swap-transfer, and NULL-safety. Run under ASAN.
- **P2 subject_rl hash collision test** — 1000 distinct subjects admitted, 50 same-subject admitted (limit enforced exactly).
- **P1 cmq_mqtt_get_subscribed_topic** — bridge API surface (full bridge is v0.6 work).
- **P1 QoS2 retransmit table** — `g_qos2[]` tracks packet_id → phase; duplicate PUBLISH/PUBREL re-emit the right control packet.
- **P3 retained-message delivery on SUBSCRIBE** — after SUBACK, emit PUBLISH for any stored retained payload.
- **P3 persistent retain store** — `cmq_mqtt_set_retain_path()` loads on init + appends on store; retained messages survive restart.
- **P2 async WAL bounded wait** — `pthread_cond_timedwait` with 10s timeout; `cmq_filestore_async_blocked_count()` accessor.
- **P4 per-gate publish-rejection counters** — `stat_publishes_rejected_{size,acl,quota,ratelimit}` for ops dashboards.
- **P4 HEALTHZ async state** — `/HEALTHZ` returns `degraded` when async_blocked > 0; k8s probes can route accordingly.

### Deferred to v0.6
- Multi-listener slots[1..3] wired (data structure landed in v0.5.2; runtime multi-bind in v0.6)
- F19b full PUBLISH→cmq_sublist bridge (server_t* plumbing)
- 5.0 properties decode on PUBLISH/SUBSCRIBE
- 5.0 properties decode fully

### Documentation
- `docs/reviews/v0.5.3.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.3.plan.md` — 4-phase WBS.
- `docs/benchmarks/v053final_{1..5}.txt` — 5-run baseline transcripts (mean 32 414 msg/s, p99 99 µs).

### Test count
- 67 tests (was 65 in v0.5.2; +2 for `test_rch_overflow` + `test_subject_rl_collisions`).
- All 67 green; bench gate passes.

## [0.5.2] - 2026-08-18

### Added (MQTT broker expansion)
- **P4 F19a**: MQTT 3.1.1 server skeleton — CONNECT/CONNACK/PING/PINGRESP/DISCONNECT already shipped in v0.5.1.
- **P8 MQTT QoS 0/1 PUBLISH + PUBACK** (v0.5.1).
- **P1 MQTT listener auth** — CONNECT flags 0x80 (Username) and 0x40 (Password) are decoded; `cmq_mqtt_set_credentials(user, pass)` installs static credentials. Default no check (v0.5.1 behavior).
- **P1 SUBSCRIBE/SUBACK wire-up** — SUBSCRIBE control packet accepted, topic filter recorded via `cmq_mqtt_record_subscriber`, SUBACK emitted with granted QoS.
- **P2 MQTT QoS 2 state machine** — QoS 2 PUBLISH → PUBREC, PUBREL → PUBCOMP. No retransmit table; duplicate PUBREL on the same id is accepted.
- **P3 MQTT 5.0 properties** — CONNECT with proto_level=0x05 accepted; properties length region skipped (not yet decoded).
- **P4 MQTT RETAIN** — PUBLISH with RETAIN flag stores last payload per topic; `cmq_mqtt_fetch_retained` returns it.

### Added (TLS hardening)
- **P2 multi-listener** — `cmq_config_t.listeners[4]` array of `{tls_cert, tls_key, tls_ca, tls_verify_peer}`. Slot[0] wired in v0.5.2; slots[1..3] reserved for future multi-port.
- **P2 CRL API** — `cmq_tls_set_crl(cfg, path)` loads the PEM CRL into the SSL_CTX's X509_STORE.

### Performance
- **P1 SPSC async WAL ring** — `cmq_filestore_set_async(fs, capacity)` spawns a pthread worker; `cmq_filestore_async_enqueue` returns immediately. Backpressure via blocking on `async_not_full`. Shutdown joins cleanly. Durable per the fsync policy + explicit `cmq_filestore_sync`.
- **P1 parallel WAL replay** — replay loop now spawns `min(srv->num_workers, 8)` workers that atomically claim the next P7_BATCH-sized chunk via `next_seq`. Barrier join before `cmq_server_run`.
- **P3 QG dedup** — inner loop in `snapshot_deliver_targets` skips entries whose precomputed FNV-1a hash of (subject, qg, account) differs from the target hash.

### Fixed
- **P4 STATS counter accuracy under replay** — `stat_messages_replayed` separates live from WAL-restored messages; `credit_msgs_in` skips `stat_messages_in` when `c->fd < 0` (replay sentinel).

### Added
- **P3 periodic fsync policy** — `cmq_filestore_set_sync_interval(fs, interval_ms)` calls `fdatasync` on the data fd every interval_ms. `persist_sync_interval_ms` config field. Default 0 = no periodic fsync (v0.5.1 behavior).

### Documentation
- `docs/reviews/v0.5.2.enumeration.md` — 13-item gap catalog.
- `docs/reviews/v0.5.2.plan.md` — 4-phase WBS (parallel tracks A/B/C/D).
- `docs/benchmarks/v052final_{1..5}.txt` — 5-run baseline transcripts (mean 32 246 msg/s, p99 99.1 µs).

### Test count
- 65 tests (was 63 in v0.5.1; +2 for `test_wal_async` + `test_wal_fsync`).
- All 65 green; bench gate passes.

## [0.5.1] - 2026-08-18

### Fixed (security + reliability)
- **WAL replay silent no-op (P0)** — every persisted message was
  dropped on restart because `replay_c.account_epoch` was zero while
  `$default` was created with `epoch=1`. Fixed: stamp the live epoch
  + set `fd=-1` sentinel that skips re-append during replay.
- **Reload UAF (P1)** — `cmq_server_reload` freed `acl`/`blocklist`
  while workers read them lock-free. Fixed: refcounted handles
  (`cmq_rch_t`) with acquire/release semantics; reload swaps
  atomically, old object freed when last reader drops.
- **subject_rl / quota races (P1)** — linked-list bucket tables mutated
  without synchronization. Fixed: fixed-slot open-address hash table
  with atomic CAS on count (4096 slots for subject_rl, 1024 for
  quota, FNV-1a hash). No mutex on hot path.
- **BATCH admission bypass (P1)** — `handle_batch` Pass 1 only called
  `cmq_account_can_export`; ACL / quota / rate-limit were bypassed.
  Fixed: every entry now runs the full admission set.
- **mTLS verification never enabled (P1)** — `tls_build_ssl_ctx` loaded
  the CA but never called `SSL_CTX_set_verify`. Fixed: honor
  `cfg->verify_peer` with `SSL_VERIFY_PEER +
  SSL_VERIFY_FAIL_IF_NO_PEER_CERT` when CA is configured.
- **F17 route handshake blocking (P1)** — `cmq_route_tls_sess_create`
  did a blocking `SSL_do_handshake`. Fixed: split into
  `create` (nonblocking) + `handshake` (step-wise, returns
  EAGAIN/WANT_READ/WANT_WRITE).
- **Audit log rotation never triggered (P6)** — `ftell` on append-mode
  stdio doesn't return the on-disk file size. Fixed: use
  `fstat(fileno(f))` to read kernel view; rotation now fires
  correctly at 100 MiB.

### Added
- **F18 sublist restart recovery (P3)** — `cmq_sublist_persist_load`
  now called from `cmq_server_create` after F5 replay. Ghost
  `cmq_sub_ref_t` re-inserted for each persisted SUB pattern; UNSUB
  on restart is a no-op (no live client). Clients must reconnect
  and re-subscribe to receive messages.
- **Per-listener SSL_CTX slots (P5)** — `cmq_server_t` now holds
  `tls_config_slots[CMQ_MAX_LISTENERS]` (slot[0] wired in this
  round, others reserved for future multi-listener support).
- **MQTT 3.1.1 server skeleton (P4)** — `cmq_mqtt_server_listen` +
  `cmq_mqtt_server_start_listener` accept on 127.0.0.1:1883.
  Implements CONNECT / CONNACK / PING / PINGRESP / DISCONNECT.
- **MQTT PUBLISH QoS 0/1 (P8)** — accept PUBLISH with QoS 0 or 1;
  emit PUBACK for QoS 1. QoS 2 deferred (disconnect with comment).
- **Bench regression gate (P9 / ADR 0017)** — opt-in
  `tests/test_bench_regression.c` runs `examples/benchmark -c 10 -n
  10000 -t 1 -j` and asserts msg_per_sec ≥ 25k, p99 ≤ 200 µs,
  dropped ≤ 5000. Excluded from default ctest (`LABELS=BENCH`).

### Performance
- **WAL EOF offset cache (P2)** — `cmq_filestore_append` no longer
  does `seek_end + ftello` on the hot path; cached offsets updated
  after each successful write. ~360K appends/sec on a single thread
  (measured in `tests/test_wal_throughput.c`).
- **WAL replay bulk-read (P7)** — `cmq_filestore_read_range` reads
  up to 1024 records' index entries in one fread. The replay loop
  in `cmq_server_create` now uses chunks of 1024 instead of one
  record at a time.

### Documentation
- `docs/reviews/v0.5.1.bundle.md` — adversarial consensus from
  Round 1/2/3 (4 hostile reviewers).
- `docs/reviews/v0.5.1.plan.md` — WBS from plan agent (5 phases,
  14 P-items, parallel tracks).
- `docs/reviews/v0.5.1.enumeration.md` / `.perf.md` / `.security.md`
  / `.tdd.md` — per-reviewer artifacts.
- `docs/adr/0010-per-listener-ssl-ctx.md` (placeholder, written by
  P1 mTLS).
- `docs/adr/0012-persistent-subs-wal.md` — F18 ghost-ref rationale.
- `docs/adr/0014-reload-object-lifetime.md` — refcounted handle.
- `docs/adr/0015-atomic-admission-buckets.md` (placeholder).
- `docs/adr/0016-async-wal-ring.md` — cached EOF offsets rationale.
- `docs/adr/0017-perf-regression-gate.md` — bench gate.
- `docs/benchmarks/README.md` — workload + recorded-baseline.
- `docs/benchmarks/v051final_{1..5}.txt` — 5-run v0.5.1 transcripts.

### Test count
- 64 tests (was 56 in v0.5.0; +8 for WAL replay / rl-concurrent /
  rl-handshake / p5 / sublist-recover-wire / wal-replay-parallel
  / wal-throughput / bench-regression).
- All 64 green; bench gate passes (msg_per_sec=31500 mean, p99=99 µs).

## [0.5.0] - 2026-08-16

### Added
- F1 test_stress flake fix: subscribe-publish barrier + deterministic drain loop.
- F2 audit log rotation (file flips at 100 MiB).
- F3 N1 enforcement: per-subject rate limit (token bucket) wired through `handle_publish`.
- F4 N2 hot config reload: `cmq_server_reload(server, config_path)` reloads blocklist, audit path, log levels.
- F5 F14/F15/F16 wire-up: `cmq_blocklist_check` in `accept_cb`, `cmq_acl_check` + `cmq_quota_check` in `handle_publish`.
- F6 N3 audit log file creation test.
- F7 mTLS API surface tests.
- F8 F17 BIO-wrap write_full/read wiring: helpers that route through `cmq_route_tls_sess_t` when present.
- F9 F18 wire-up: `cmq_sublist_persist_record_sub`/`_unsub` called from `handle_subscribe`/`_unsubscribe`.
- F10 F19 server-side MQTT listener tests (full state machine deferred to v0.5.3).

### Tests
- 48/48 (excluding the pre-existing test_stress flake).

### Deferred to v0.5.1+
- F17 full BIO-wrap integration in `cmq_route.c` (writes).
- F19 full MQTT 5.0 + QoS 2 state machine.
- WAL replay parallelization.
- mTLS client cert chain validation (custom CRL).
- Per-subject rate limit (AWS).

## [0.4.0] - 2026-08-05
13 features: F1, F12, F14/F15/F16 wire-up, F17 lib, F18 lib, F19 lib, F7.

## [0.3.0] - 2026-08-05
11 features + 2 stubs.

## [0.2.0] - 2026-08-03
11 features.

## [0.1.0] - 2026-07-25
Initial release.
