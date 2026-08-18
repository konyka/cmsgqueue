# Changelog

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
