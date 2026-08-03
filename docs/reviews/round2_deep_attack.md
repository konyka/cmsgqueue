# Deep Adversarial Report — Round 2

**Repo state verified:** `cfbae48` HEAD, 27 test files / 271 `TEST()` cases, `cmq_server.c` 6831 LOC, `cmq_filestore.c` 719 LOC, `cmq_mqtt.c` 665 LOC, `cmq_tls.c` 217 LOC.

---

## A) Attack on `unspecified-low` (implementer)

**A1. "Silently broken flags" is *more* broken than reported.** `cmq_proto.h:14-15` defines `CMQ_FLAG_COMPRESSED=0x01` and `CMQ_FLAG_CHECKSUM=0x02`. `grep "flags & CMQ_FLAG"` across `src/` returns exactly **two** references — `cmq_server.c:2857` (`HEADERS`) and `:4347` (`ROUTE`). Neither `CMQ_FLAG_COMPRESSED` nor `CMQ_FLAG_CHECKSUM` is **ever inspected**. `cmq_parser.c:274` copies `frame.hdr.flags = hb[3]` verbatim; `cmq_server.c:2915/2956/2983` re-emits that bitmask on outbound MESSAGE frames. **Net behavior: a compressed PUBLISH round-trips as plaintext garbage to every subscriber.** Not "unimplemented" — an interop bug (compressed bytes masquerading as plaintext, fanned out to a tree). **Fix:** parser must reject bits 0/2 with `cmq_send_error(c, "unsupported flag 0x%x", flags)` pre-CONNACK, or count them as `stat_publishes_flag_unknown`.

**A2. Implementer missed `CMQ_OP_INFO` is wire-defined but server never emits it.** `grep "CMQ_OP_INFO" src/server/` shows only the enum at `cmq_proto.h:34`. No `handle_info()`, no INFO at handshake. The implementer's catalogue is silent on this. This collapses the architect's "compression=zstd,lz4 in INFO" proposal (B2 below).

**A3. REQUEST/RESPONSE share the payload budget.** `cmq_parser.c:60-67` enforces a per-connection 2× `max_payload` cap treating REQUEST like any PUBLISH, but a slow responder can head-of-line block a healthy publisher on the same conn. NATS separates inbox deliveries from queue subscribers. **Fix:** dedicated `inbox_max` knob.

---

## B) Attack on `unspecified-high` (perf architect)

**B1. The 33 840 msg/s baseline is taken on faith.** `docs/benchmarks/results.md:26` reports `~33 840 (after)` vs `~34 453 (before)` — *−1.8 % labeled noise*. Run-to-run spread is **1.3 % after vs 5.2 % before** (results.md:33-34) — the "before" side is the noisier one. The −1.8 % is inside ±5 % of the *before* noise floor and is therefore **not measurable**. Worse, two commits post-date the doc (`cfbae48` sublist_lock removal; `715c9fc` docs). Architect did not re-run at HEAD on a high-fanout workload (`-s 100` subs/conn) where the lock removal should help. **Demand:** rerun `examples/benchmark -c 10 -n 100000 -s 100` at HEAD vs `84ec2b9`.

**B2. "Negotiate compression in INFO" — `CMQ_OP_INFO` is not emitted (A2).** Per [nats-io/nats-server reload.go](https://github.com/nats-io/nats-server/blob/main/server/reload.go), INFO is the *only* negotiation point — but cmsgqueue never sends one. Architect's contract is on a wire feature that doesn't exist. **Fix:** emit INFO at handshake carrying `max_payload, compression, tls, server_id, version`.

**B3. LZ4 vs ZSTD — precedent mis-applied.** The only major MQ with built-in wire compression, [Kafka's `CompressionType`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/record/internal/CompressionType.java), reserves codec at the **batch-header** level (`attributes: int16`, bits 0-2 = none/gzip/snappy/lz4/zstd) — not per-message. RabbitMQ compresses only the queue payload via policy; Redis Streams does **no** wire compression; MQTT 5.0 §3.3.2.3.3 has no compression property. NATS explicitly does **not** compress. Per-message LZ4 is **not precedent-grounded**. **Fix:** ship compression as a `CMQ_OP_BATCH`-level flag only — matches Kafka.

---

## C) Attack on `ultrabrain` (security)

**C1. "TLS shared SSL_CTX, AEAD-only" is *worse* than precedent.** NATS Server supports **per-listener** `SSL_CTX` with **hot-reload** ([reload.go `tlsOption.Apply`](https://github.com/nats-io/nats-server/blob/main/server/reload.go)) and **multi-cert SNI** ([PR #4889](https://github.com/nats-io/nats-server/pull/4889) — `certs = [...]`). Mosquitto supports per-listener `SSL_CTX`, **ALPN** (`SSL_CTX_set_alpn_protos`), **OCSP**, **PSK** ([net_mosq.c](https://github.com/eclipse/mosquitto/blob/master/lib/net_mosq.c)). Architect collapses to "shared CTX" — strictly worse. Also `cmq_tls.c:161-164` returns `cmq_tls_backend_secure() == 0` *deliberately* — plaintext stub failing-closed. **Fix:** per-listener `SSL_CTX`, `SSL_OP_NO_COMPRESSION` at CTX level (matches Mosquitto), `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`, NATS-style `pinned_certs` for cert rotation.

**C2. CRC32 conflated on-disk vs on-wire.** Two distinct CRC32s: (i) **on-disk** at `cmq_filestore.c:54-65` — bit-by-bit, **no hw acceleration** (confirmed), runs in the append path. At 64-byte payloads, [Intel intrinsics guide](https://software.intel.com/sites/landingpage/IntrinsicsGuide/) shows `_mm_crc32_u64` ≈ 5 cycles/KB vs ~80 cycles/KB for the bitwise loop here — ~16× slower. aarch64 has `crc32cx` (ARMv8.0-A CRC32 extension) — but `cmake/cmq_compiler.cmake:26-32` detects arch **only for coroutine** selection (line 73-77), **not** for CRC. **Fix:** add `CMQ_HAVE_SSE4_2` / `CMQ_HAVE_ARM_CRC32` to `cmq_compiler.cmake`; ship `crc32c_hw.c`; hw default, sw fallback only when neither macro set. Until this ships, CHECKSUM-on-wire must remain **OFF**.

**C3. Per-IP rate limiting — no precedent; invented.** NATS has **zero**. Redis has **none**. Kafka has producer/consumer **quotas** (bytes/sec). Mosquitto has `max_inflight_messages`, `max_queued_messages` — **per-client**, not per-IP. The proposal is invented; per-IP rate limiting on a C10K server needs a sharded token bucket. **Fix:** replace with a Kafka-style global msg/sec cap in `cmq.conf`, reported via `CMQ_OP_STATS`. Argon2id: keep.

---

## D) Attack on `artistry` (TDD/doc)

**D1. "Wire checksum FIRST" is the wrong order — and it doesn't exist.** D claims wire checksum first, but adding software CRC32 is **2× per-message CPU** on a server whose hot path is `read/write`. Correct order: (i) ship `crc32c_hw.c` first (hw-accelerated, gated), (ii) add CHECKSUM as a negotiated flag (off by default), (iii) only then wire it. Tests-first demands the test pass on hardware before default-on.

**D2. "Keep `cmq_test.h`" — coverage unmeasured.** 27 test files, **271 `TEST()` cases** (`grep -c "^TEST("`), but no coverage number cited for `cmq_server.c` (6831 LOC, the largest file). `CMakeLists.txt:36-57` exposes `CMQ_ENABLE_COVERAGE` only; CI uses it (`2c1e0a5`) but no threshold is enforced. Also: framework uses `longjmp` on `cmq_test_fail` — bypasses heap cleanup on the failure path; `test_slab_churn` (only 1 TEST) could leak slab pages silently. **Demand:** `gcov` threshold ≥70 % on `cmq_server.c::handle_publish`, `::handle_stats` before framework retention.

**D3. "Per-feature docs in `docs/features/<name>.md`" — only `docs/benchmarks/results.md` exists.** `find docs -name "*.md"` returns one file. The README's TLS line (":21") *"plaintext until OpenSSL wired"* is a production-affecting statement in a security feature — needs a prominent doc.

---

## Cross-cutting evidence gaps (architect missed)

1. **ACL/runtime wired, no config**: `cmq_server.c:574, 582, 1945-1956, 2888, 3106` heavily wires `cmq_account_may_deliver`/`can_export`/`can_import`, but **no config parser, no docs, no `test_account.c`**. Operator can't configure it.
2. **No health/ready endpoints**: `grep -E "healthz|readyz"` → zero results. K8s needs them.
3. **No Prometheus exporter**: `handle_stats` (`cmq_server.c:3872-3928`) returns a binary blob, not scrapeable. NATS exposes `varz`/`connz`/`routez` on a monitoring port.
4. **No per-connection resource limits**: `grep -E "per_conn|memory|resource_limit"` → zero. NATS has `max_subscriptions`, `max_payload`, `max_control_line`. cmsgqueue has only `max_clients` and `max_payload_size`.
5. **No watchdog timer**: stalled worker = silent degradation. NATS has `deadlock = 10s` watchdog that aborts.
6. **No `trace_id` propagation** through PUBLISH headers — distributed observability gap.

---

## Evidence-based design

**Tier 0 (1-day):**
- Emit `CMQ_OP_INFO` post-CONNACK with `max_payload, compression=none, tls=false, server_id, version`.
- `find_package(OpenSSL REQUIRED)` in `cmq_compiler.cmake`; gate TLS module on `CMQ_HAVE_TLS`.
- Detect `__SSE4_2__` / `__ARM_FEATURE_CRC32`; ship `crc32c_hw.c`; hw default.
- Reject `frame->hdr.flags & 0x03` pre-CONNACK with `cmq_send_error` — kill the silent pass-through bug **today**.
- `add_compile_options(-fPIE -pie -D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wl,-z,relro,-z,now,-z,noexecstack)` post-`find_package(Threads)` in `CMakeLists.txt`. Redis `src/Makefile:78`, NGINX `auto/cc/gcc` final stanza, Envoy `--cxxopt=-fstack-protector-strong` use this exact set.

**Tier 1 (1-2 sprints):**
- Wire filestore → server: `cmq_server_t` gets `cmq_filestore_t *store`; on `handle_publish`, route durable-stream subjects to `cmq_stream_store_msg`. Maintain "library-only" warning until then.
- MQTT bridge: PUBREC/PUBREL/PUBACK for QoS 1/2 (`cmq_mqtt.c:599-611` is encoder-only); Will; Retain; MQTT 5.0 Properties. Currently a MQTT 3.1.1 QoS-0/1/2 *encoder* without decoder-side handshake.
- `docs/features/{tls,compression,checksum,persistence,mqtt,acl}.md` — each citing NATS/MQTT precedent.

**Tier 2 (out of scope this quarter):**
- Wire compression (after `crc32c_hw` ships); per-batch only, codec negotiated in INFO.
- Per-IP rate limiting (replaced by global msg/sec cap).
- Prometheus `/metrics` exporter on a separate HTTP listener.