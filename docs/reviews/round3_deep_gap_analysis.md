# CMSGQueue v0.2.0 — Evidence-Based Gap Analysis

**Roster position:** deep (evidence + precedent researcher)
**Repo state:** HEAD `c386ad4`, tag `v0.2.0`, 11 features shipped, ~13.8 KLOC src / ~7.9 KLOC tests
**Inputs cross-checked:** `hyperplan-bundle.md`, `hyperplan-final-plan.md`, `docs/benchmarks/results.md`, `CHANGELOG.md`, all 12 `docs/features/*.md`, local source grep for F1–F15, 279 individual TEST() entries across 36 files.
**Authoritative citations:** OWASP Password Storage Cheat Sheet (raw markdown), LevelDB log format docs, PostgreSQL WAL docs, libsodium docs, NATS official docs, Confluent Kafka docs, Mosquitto/NanoMQ source repos.

---

## 1. NATS Feature Parity Gap

NATS has 6 distinct subsystems CMSGQueue only partially covers or doesn't ship at all. Verified against `docs.nats.io` and `github.com/nats-io/nats-server`.

| Feature | NATS | CMSGQueue v0.2.0 | File:line evidence | Effort |
|---|---|---|---|---|
| **Accounts / Users / Permissions** | First-class. Per-account connection/sub/memory/data limits; user-level pub/sub/conn limits enforced atomically. `accounts.go` (~2 KLOC). | **Partial.** `cmq_account_t` + `cmq_account_manager_t` ship; export/import ACL semantics implemented (22/22 tests pass). **Missing:** per-account limits (`max_connections`, `max_subs`, `max_payload`, `max_data`), user-level limits (pub/sub/conn), account-scoped stats counters integration. | `src/enterprise/cmq_account.c:990`; `cmq_account.h:25-50`; no `max_connections` field anywhere in struct | **M** (4 KLOC + tests) |
| **Decentralized JWT auth (NKEY + signed JWTs)** | Ed25519 keypairs per operator/account/user, chain-of-trust via signed JWTs, OCSP-style revocation. `jwt.go`. | **Missing entirely.** `auth_username` / `auth_password` plaintext in config (`strdup`-lifetime), `src/server/cmq_config.c:124-125, 129-130`. No Ed25519, no JWT. | grep `JWT\|NKEY\|Ed25519` src/ → 0 matches | **XL** (deps: libsodium or openssl for Ed25519, jwt-c library) |
| **Subject mapping & transforms** | Per-account streams; map `foo.*` → `bar.*` across accounts. | **Partial.** Export/import covers the cross-account case; **no per-account stream mapping** (publish-side subject rewriting). | grep `map_subject\|rewrite_subject` src/ → 0 matches | **M** |
| **Leaf nodes** | Edge-to-hub tier; partial-mesh leaf. `leaf.go` (1.3 KLOC). | **Code present but documentation marks as "wired"**. `src/cluster/cmq_leaf.c:1366` LOC, but the bundle §1 declares leaf/route/gateway "fully wired" (`cmq_server.c:6218-6228`). The library code exists; **integration test is missing** (`grep` for `leaf` in `tests/` returns only `test_cluster.c` cluster-routing, not leaf). | `src/cluster/cmq_leaf.c`; tests/test_cluster.c:332 only has `cmq_cluster`, not `cmq_leaf` | **S** (test wiring only) |
| **Cluster routing (supercluster)** | Mesh of nodes, gossip protocol. | Code present (`cmq_route.c:1741` LOC); tests in `test_cluster.c`. **Gap: no end-to-end multi-process integration test.** | tests/test_cluster.c | **S** |
| **Gateways** | Independent cluster interconnect. | `cmq_gateway.c:885` LOC; same gap as leaf — no end-to-end test. | tests/ | **S** |
| **JetStream / KV / Object Store** | File-backed streams, consumer groups, KV buckets (linearizable), Object Store (S3-like), `nuid`. `jetstream.go` ~50 KLOC. | **Entirely missing.** grep `JetStream\|jetstream\|KV\|key_value\|object_store\|consumer_group` src/ → 0 hits outside of internal-staff-notebooks (`notes.html`). The closest analog is `cmq_stream.c:443` (raw byte ring buffer, not consumer-group semantics). | grep confirms 0 matches | **XXL** (months of work; rightly out of scope for v0.2.x) |
| **Message tracing & monitoring** | Distributed-tracing spans (OpenTelemetry), `varz` endpoint, `accounts` trace, `leafz`, `gatewayz`, `routez`, `subz`, `connz` introspection. `monitor.go`. | `/healthz`, `/readyz`, `/metrics` exist (F12/F13) but **no per-account/conn/sub introspection** (no `connz`/`subz`/`routez`). grep `connz\|subz\|routez` → 0. | docs/features/health-metrics.md | **M** |
| **Performance numbers** | `nats bench` reports **~10–14M msg/s** on a single core (1KB pub/sub, localhost). Dereks Collison's gist (canonical): 14M msg/s on AWS c5.xlarge. Redis Streams: 1M+ ops/s. Kafka: 100K+ msg/s per partition (LinkedIn benchmark: 2M/s with 3 hosts × 3 partitions). | **~30K msg/s end-to-end.** 333× slower than NATS, 33× slower than Redis Streams, 3× slower than Kafka per partition. See §7. | docs/benchmarks/results.md:26 | **XL** |

**Sources.**
- https://docs.nats.io/running-a-nats-service/configuration/accounts
- https://docs.nats.io/running-a-nats-service/configuration/securing_nats/auth_intro/jwt
- https://docs.nats.io/running-a-nats-service/clustering
- https://docs.nats.io/nats-concepts/jetstream
- https://gist.github.com/derekcollison/4616831fc476e5f2117e (NATS 14M msg/s gist)
- https://docs.nats.io/using-nats/nats-tools/nats_cli/natsbench

---

## 2. Kafka Feature Parity Gap

Kafka is a much heavier system; not all features map 1:1. The ones that matter for an at-least-once pub/sub broker:

| Feature | Kafka | CMSGQueue v0.2.0 | Effort |
|---|---|---|---|
| **Log compaction** (segment-by-key) | YES. Cleaner threads scan segments, keep latest value per key; tombstones retained for `delete.retention.ms` (default 24 h); `min.cleanable.dirty.ratio` defaults to 0.5. Reference: Confluent "Log Compaction" docs. | **Missing.** `cmq_stream.c` is an append-only ring buffer with TTL-style aging only (none documented). No key-aware compaction. | **L** |
| **Exactly-once semantics** | YES. Idempotent producer (`enable.idempotence=true`, PID+sequence), transactional producer with commit/abort markers, `read_committed` consumer isolation. | **Missing.** grep `EOS\|exactly_once\|idempotent` → 0 hits. No PID, no sequence numbers per producer. | **XL** |
| **Quotas (storage, network)** | YES. Per-principal storage/network quota (`quota.producer.default`, `quota.consumer.default`, `quota.window.num`, `quota.window.size.seconds`). Reference: kafka.apache.org/documentation/#design_quotas. | **Partial.** F10 ships per-IP connect rate-limit + per-conn subject cap. **No per-principal bandwidth quota.** | **M** |
| **Tiered storage** (KIP-405, Kafka 3.6+) | YES. Local + remote (S3/HDFS) tiers; `log.segment.local.retention.ms`, hot/warm/cold lifecycle. | **Missing.** No remote-tier plumbing; only local `cmq_filestore`. | **XL** |
| **Transactions (coordinator)** | YES. `__transaction_state` topic, transaction coordinator, fencing tokens. | **Missing.** No transaction coordinator, no commit/abort marker semantics. | **XL** |

**Sources.**
- https://kafka.apache.org/documentation/#log_compaction
- https://kafka.apache.org/documentation/#semantics
- https://kafka.apache.org/documentation/#design_quotas
- https://kafka.apache.org/documentation/#tiered_storage
- https://kafka.apache.org/documentation/#transactions
- https://docs.confluent.io/platform/current/kafka/design/log_compaction.html
- https://engineering.linkedin.com/kafka/benchmarking-apache-kafka-2-million-writes-second-three-cheap-machines

**Realistic CMSGQueue aspiration from Kafka:** F10 (rate limit, partial) + log compaction + bandwidth quota. Skip EOS / tiered / transactions in v0.x — those need a stable partition model CMSGQueue does not have.

---

## 3. MQTT Server-Side Implementation

CMSGQueue ships `cmq_mqtt.c` encoder-only (10 tests, all encode-side). Wiring a real broker requires a `parse → dispatch → publish` loop. Three MIT/EPL reference brokers:

| Broker | Repo | License | LOC (approx) | Threading | Persistence | State machine |
|---|---|---|---|---|---|---|
| **Eclipse Mosquitto** | github.com/eclipse/mosquitto | **EPL/EDL (NOT MIT)** | ~30 KLOC C | Single-thread broker loop + per-client thread pool for bridge/HTTP | `mosquitto.db` (custom BDB-like format), append-only, rotated | `mosquitto.c` main loop; protocol state in `MqttState` struct |
| **NanoMQ** | github.com/emqx/nanomq | **MIT (Apache 2.0 for parts)** | ~50 KLOC C + Erlang-ish Mria | Multi-node Mria: each node runs directory + forwarder threads; cross-node pub/sub via Mnesia-like | Custom `db_bdb`-style files, WAL-based | `mqtt_bridge.c`, `nng` core for socket |
| **Mongoose MQTT** | github.com/cesanta/mongoose | **GPLv2 + commercial** | ~40 KLOC C (embedded) | Single-threaded event loop, no threading | None (in-memory) | `mg_mqtt.c` state-machine handler |
| **wolfMQTT** | github.com/wolfSSL/wolfMQTT | **GPLv2 OR commercial** | ~10 KLOC C (client-leaning) | N/A (client) | N/A | `wolfmqtt/mqtt_client.c` |

**Best reference for CMSGQueue:** **NanoMQ**. MIT license, in C, walks parse → match → publish with a clear single-node directory model. Specifically:
- `nanomq/apps/src/pipe.c` (per-connection mqueue)
- `nanomq/apps/src/sub_handler.c` (subscriber dispatch)
- `nanomq/nng/src/sp/protocol/mqtt/mqtt.c` (protocol frames)

**What CMSGQueue needs to wire** (pattern from NanoMQ):
1. Subscribe-side: per-conn QoS 1/2 **inflight** mqueue (`struct inflight { uint16_t packet_id; uint8_t qos; time_t expiry; }`).
2. Publish-side: PUBLISH acks (PUBACK for QoS 1; PUBREC/PUBREL/PUBACK handshake for QoS 2).
3. Broker-level: SUBACK with granted-QoS handling, UNSUBACK.
4. Lifecycle: `will` (last-will-and-testament) stored in `cmq_account_t` or per-conn struct.
5. Persistence: extend `cmq_filestore` to be the Mosquitto-style session-state WAL (subscribed topics + inflight packet IDs).

**License-pick rationale:** Mosquitto (EPL) is **NOT** compatible with a permissive Apache/BSD codebase without source disclosure. Use NanoMQ (MIT) as reference; copy patterns (not code).

**Sources.**
- https://github.com/eclipse/mosquitto
- https://github.com/emqx/nanomq
- https://github.com/cesanta/mongoose
- https://github.com/wolfSSL/wolfMQTT

---

## 4. Password Hashing Standards (F8)

**Authoritative source:** OWASP Password Storage Cheat Sheet (raw markdown fetched). Direct citation:

> "Use Argon2id with a minimum configuration of **19 MiB of memory, an iteration count of 2, and 1 degree of parallelism.**
> If Argon2id is not available, use **scrypt** with a minimum CPU/memory cost parameter of (2^17), a minimum block size of 8 (1024 bytes), and a parallelization parameter of 1.
> For legacy systems using **bcrypt**, use a work factor of **10 or more** and with a password limit of 72 bytes.
> If FIPS-140 compliance is required, use **PBKDF2** with a work factor of **600,000 or more** and set with an internal hash function of HMAC-SHA-256.
> Consider using a **pepper** to provide additional defense in depth."

| Algorithm | Parameters (OWASP 2024-2026) | Standard | Library call | Side-channel | GPU hardness |
|---|---|---|---|---|---|
| **Argon2id** | m=19 MiB, t=2, p=1 | RFC 9106 (winner PHC 2015) | `libsodium: crypto_pwhash_str_alg(..., crypto_pwhash_ARGON2ID13, crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE)` (libsodium's interactive = m=64 MiB, t=2, p=1 — even stronger than OWASP minimum) | High (data-independent memory access — Argon2i side; Argon2id hybrid) | High (memory-hard) |
| **scrypt** | N=2^17 (131 072), r=8, p=1 | RFC 7914 | `OpenSSL: EVP_PBE_scrypt` or `libsodium: crypto_pwhash_scryptsalsa208sha256_str` | Medium (cache timing possible) | High (memory-hard) |
| **bcrypt** | cost ≥ 10, 72-byte input cap | Provos-Mazières 1999 | `OpenSSL: BCrypt_gensalt + BCrypt_hashpw` (in `libcrypt` on Linux, BINDCRYPT) | Medium | Medium (not memory-hard; only compute-hard) |
| **PBKDF2-HMAC-SHA256** | iterations ≥ 600 000 | RFC 2898 | `OpenSSL: PKCS5_PBKDF2_HMAC` / `libsodium: crypto_pwhash` | Lower (cache timing) | Low (no memory hardness) |

**Recommendation for CMSGQueue F8 (concrete):**
- Use **libsodium `crypto_pwhash_str_alg` with ARGON2ID13 at INTERACTIVE ops/mem limits** (gives `m=64 MiB, t=2, p=1`, stronger than OWASP minimum). No tuning required, version tag in output, constant-time compare built-in.
- Migration path: when loading `auth_password`, detect by `$argon2id$` prefix → `crypto_pwhash_str_verify`; otherwise treat as legacy plaintext and **emit a one-shot deprecation warning** to stderr at startup, then still accept it (CWE-256 retro-fix). Add a `auth_password_hash` separate config field for the hashed form; `auth_password` (plaintext) marked deprecated in `docs/`.
- Optional pepper: read a 32-byte file path from `auth_pepper_file` (mode 0400), prepend to password before hashing. Cited from OWASP "peppering" section.

**Sources.**
- https://raw.githubusercontent.com/OWASP/CheatSheetSeries/master/cheatsheets/Password_Storage_Cheat_Sheet.md
- https://datatracker.ietf.org/doc/html/rfc9106 (Argon2)
- https://datatracker.ietf.org/doc/html/rfc7914 (scrypt)
- https://datatracker.ietf.org/doc/html/rfc2898 (PBKDF2)
- https://doc.libsodium.org/password_hashing/default_phf

---

## 5. TLS Best Practices (F1)

Mozilla's profile guide (now at `tlsref.org` after wiki.mozilla.org migration):

- **Modern profile:** TLS 1.3 only, AEAD-only ciphers, ECDHE + RSA-PSS / Ed25519. No TLS 1.2 fallback.
- **Intermediate profile (recommended default for CMSGQueue):** TLS 1.2 minimum, TLS 1.3 preferred, 3DES/RC4/RC2/DES/SEED/MD5 disabled.
- **Old profile:** legacy (avoid).

**Cipher suite list (TLS 1.3, RFC 8446 §B.4):** prefer `TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256` (server-preference order).

**TLS 1.2 ECDHE + AEAD (Mozilla intermediate):**
`ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256`.

**OpenSSL 3.x API calls (concrete, recommended):**
```c
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_set_ciphersuites(ctx, "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256");
SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20"); // TLS 1.2
SSL_CTX_set1_curves_list(ctx, "X25519:P-256:P-384");
SSL_CTX_set_security_level(ctx, 2); // or 3 for FIPS
SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
```

**mTLS (service-to-service):**
```c
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
SSL_CTX_load_verify_locations(ctx, CAfile, CApath);
```
Optional: `SSL_CTX_set_verify_depth(ctx, 4)`.

**HSTS** (HTTP management endpoints /healthz, /readyz, /metrics):
`Strict-Transport-Security: max-age=63072000; includeSubDomains; preload`

**OCSP stapling:**
`SSL_CTX_set_tlsext_status_cb(ctx, ocsp_cb)` for per-handshake async stapling; use `OCSP_basic_verify` with `OCSP_TRUSTOTHER` for soft-fail (operational realism).

**Performance:** disable session tickets (`SSL_OP_NO_TICKET`) or set short ticket keys rotated every 1h; server-side session cache with `SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER)`. Avoid 0-RTT (replay risk for broker auth).

**Gap in CMSGQueue F1:** The CHANGELOG says "AEAD-only ciphers, TLS 1.2 floor" — verified greenfield. No `tests/test_tls.c` exists; `grep TLS|tls_ cipher|mTLS|mutual tests/` returns 0 hits in dedicated TLS test file (only generic INFO capability test). **Add `tests/test_tls_handshake.c` with OpenSSL `SSL_CTX_new`-driven client/server loop.**

**Sources.**
- https://wiki.mozilla.org/Security/Server_Side_TLS (legacy) → https://wiki.mozilla.org/index.php?title=Security/Server_Side_TLS
- https://datatracker.ietf.org/doc/html/rfc8446 (TLS 1.3)
- https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_status_cb/
- https://docs.openssl.org/3.0/man3/SSL_CTX_set1_curves_list/

---

## 6. WAL Recovery Patterns

| System | Record format | Max record | fsync policy | Recovery | Compaction |
|---|---|---|---|---|---|
| **LevelDB** | 32 KiB blocks, records: `crc32c:u32 \| len:u16 \| type:u8 \| data[u8;len]`. Types FULL/FIRST/MIDDLE/LAST. 7-byte trailer per block (zero-fill). | Spans multiple blocks via FIRST/MIDDLE/LAST (no hard cap, but ~block-size single record is the norm) | Group commit (`sync=false` per Put, sync per `WriteOptions.sync=true` write). Per-manifest fsync at compaction. | Read log; on checksum mismatch skip to next block boundary; replay in order. | SSTable-level compaction (Level-0 → Level-N). Log itself is not "compacted" — rotated at 4 MiB. |
| **Kafka** | Variable-length record: `offset:u64 \| length:u32 \| crc32:u32 \| magic:u1 \| attrs:u1 \| timestamp:i64 \| key_len+key \| val_len+val \| headers`. | `message.max.bytes` default **1 MiB**; broker-side `max.message.bytes` default 1 MiB. | Group commit: `log.flush.interval.messages=N` and `log.flush.interval.ms=M`. acks=0/1/all. | On startup, scan each segment, verify CRC32, **truncate to last valid offset**. Index rebuilt lazily. | **Log compaction**: keep latest value per key, retain tombstones for `delete.retention.ms`. Triggered by `min.cleanable.dirty.ratio`. |
| **PostgreSQL** | WAL records in **16 MiB segment files**, each holds 8 KiB pages with full-page writes, LSN-mono address. | One record header (24 B) + data; large records span pages; max record size effectively ~1 GB. | `fsync=on` (default), `wal_sync_method=fdatasync` or `open_sync`; `synchronous_commit=on`; group commit via `commit_delay`/`commit_siblings`. | REDO from last checkpoint: read pg_control → last checkpoint → replay every record forward. | Continuous WAL recycling (not compaction); segment file renamed to `.partial` after checkpoint. No per-record compaction. |
| **CMSGQueue `cmq_filestore`** | Fixed 64-byte entries (`cmq_filestore.c:54-65`), header + payload, software CRC32C. | 64 B per entry — **no large-record support**. | `cmq_filestore_sync` on shutdown only; no per-record fsync; best-effort. | `cmq_filestore_last_seq` then `cmq_filestore_read` per seq. **No checksum verification on replay.** | None. Append-only, no rotation, no compaction. |

**Gap diagnosis (CMSGQueue vs the three):**
1. **Max record size**: 64 B → effectively unusable for any payload > ~32 B. Must move to `len:u32 header + payload + crc32c:u32 trailer` like LevelDB.
2. **Checksum verification on replay**: absent — silent corruption is possible.
3. **No rotation/compaction**: long-running broker's `cmq.data` grows unbounded; no `min.cleanable.dirty.ratio` analog.
4. **Recovery is synchronous in `cmq_server_create`** (docs/features/persistence.md) — no checkpoint → O(N) replay on every restart.

**Concrete roadmap:**
- Lift record format from LevelDB log format (proven, has CRC, handles arbitrary record sizes).
- Add `cmq_filestore_compact()` with Kafka-style key-prefix compaction (or simpler: time-based rotation).
- Add `cmq_filestore_checkpoint(seq)` → write sparse index; recovery starts from checkpoint.

**Sources.**
- https://raw.githubusercontent.com/google/leveldb/main/doc/log_format.md
- https://docs.confluent.io/platform/current/kafka/design/log_compaction.html
- https://www.postgresql.org/docs/current/wal-intro.html
- https://www.postgresql.org/docs/current/wal-configuration.html

---

## 7. Performance Gap

| System | Throughput | Source |
|---|---|---|
| NATS Server (1 KB pub/sub, localhost, 1 core) | **~10–14M msg/s** | https://gist.github.com/derekcollison/4616831fc476e5f2117e ; https://docs.nats.io/using-nats/nats-tools/nats_cli/natsbench |
| Redis Streams (XADD burst) | **~1M ops/s** | https://devopedia.org/redis-streams |
| Kafka (per partition, default) | **~100K msg/s** | https://engineering.linkedin.com/kafka/benchmarking-apache-kafka-2-million-writes-second-three-cheap-machines (2M/s on 3×3 partition cluster) |
| **CMSGQueue v0.2.0 (e2e, 10 pub × 1 sub)** | **~30K msg/s** | docs/benchmarks/results.md:26 |

**Multiplier:** NATS / CMSGQueue ≈ **300–460×**. Kafka / CMSGQueue ≈ 3×.

**Where the time goes in CMSGQueue** (educated from public benchmarks + local reading of cmq_parser.c / cmq_server.c):
- **`read()`/`write()` syscalls**: 200–500 ns each. Without `MSG_ZEROCOPY`/`splice`, every message costs 2 syscalls minimum.
- **Mutex contention** on `sublist_mu`, `conn_mu`, `q_lock`: ~50–200 ns when uncontended, 1–10 µs contended. CMSGQueue uses a global sublist mutex (`cmq_sublist.c`); NATS uses sharded per-token sublists.
- **`malloc`/`free` for every frame**: even with the new slab/ mpool from v0.2.0, glibc malloc adds 30–100 ns per call.
- **Hash-table traversal** in `cmq_sublist_match`: O(log N) per token; with N subjects × N subscribers, the dispatch path is the dominant cost.
- **fsync on persist**: not on the read path but on writes — slows publish path by ~10 µs per publish when persistence enabled (not in benchmark).

**Why NATS is ~300× faster:**
1. **Single-threaded GOMAXPROCS=1 with no mutex in hot path** (Go's channels + per-subject lock-free skip-list). CMSGQueue has a per-conn mutex that gets acquired on every cross-worker SEND.
2. **io_uring (Linux 5.10+) or epoll with batched submits.** NATS uses `runtime.netpoll` (Go internal poll).
3. **Zero-copy where possible.** NATS avoids `memcpy` of payloads by reference-counting buffers.
4. **No per-msg heap allocation** (Go's escape analysis + freelist).
5. **Sharded subject tree** vs CMSGQueue's global sublist.

**Realistic path for CMSGQueue to 100K–1M msg/s:**
- **S** Implement `cmq_sublist_shard` — split sublist into 16 hash-sharded sub-trees.
- **S** Replace per-conn mutex with per-conn mbox (MPSC) + worker drain loop.
- **M** Add Linux `io_uring` backend (conditional compile, fallback to epoll).
- **M** Multi-worker pool with `cmq_worker_id` encoded in PUBLISH header → no cross-worker sync for same-subject fan-out.
- **S** Batch `writev()` for subscriber flush.
- **XL** Zero-copy payload: ref-counted `cmq_msg` with `release()` instead of `free()`.

**Sources.**
- https://gist.github.com/derekcollison/4616831fc476e5f2117e
- https://docs.nats.io/using-nats/nats-tools/nats_cli/natsbench
- https://devopedia.org/redis-streams

---

## 8. Test Coverage Gap

**Inventory:** 36 test files, 279 individual TEST() entries.

**What's covered (good):**
- Core: atomic, queue, slab, mpool, parser, parser_backpressure, sublist (incl. contention), worker, coro, ev (event loop), log.
- v0.2.0 features: checksum_wire, compress, crc32c, enterprise (account, tls, mqtt, ws), hardening, health_metrics, inbox, info, rate_limit, recover, persist_unit, store.
- Integration: cluster, request_reply, integration, phase2, server_ops, server, stress, queue_race, slab_churn, sublist_contention.

**What's missing (gaps):**

| Gap | Severity | Why it matters |
|---|---|---|
| **No fuzz harness for parser** (`test_parser.c`, `test_parser_backpressure.c`) | **High** | CMSGQueue's wire protocol is the trust boundary. No fuzz coverage means parser regressions ship undetected. Reference: libFuzzer `LLVMFuzzerTestOneInput` harness would take ~50 LOC. |
| **No `tests/test_tls.c`** (only `test_enterprise.c` has TLS config tests, no handshake) | **High** | F1 ships but no end-to-end TLS handshake test. Use `OpenSSL s_server`/`s_client` or in-process `BIO_*`. |
| **No multi-process cluster integration test** | **Medium** | `test_cluster.c` exercises `cmq_cluster_*` primitives, but no test spins up 3 server processes and verifies route propagation. |
| **No leaf node test** (`grep leaf tests/ → 0`) | **Medium** | Bundle §1 declares leaf "fully wired" but there's no test invoking `cmq_leaf_*`. |
| **No gateway test** (`grep gateway tests/ → 0`) | **Medium** | Same. |
| **No MQTT broker test (full parse → dispatch → publish)** (`tests/test_enterprise.c` only encodes) | **High** | F6 (mqtt-bridge) ships bridge stubs but no test of the broker side. |
| **No persistence recovery end-to-end test** (`tests/test_recover.c` only tests the filestore library, not server restart) | **Medium** | docs/features/persistence.md admits: "End-to-end (server-create + restart) is tested manually because the test framework cannot easily fork+restart a server." A `fork()`-based recovery test is ~30 LOC. |
| **No ASAN/UBSAN CI gate** (`grep -n sanitize .github/workflows/`) | **Medium** | `tests/CMakeLists.txt` has ASan/UBSan options but no CI runs them. |
| **No JWT / NKEY test** (post-F8, when implemented) | **Future** | |
| **No throughput regression gate** | **Medium** | `bench/` has `benchmark.c` but no CI assertion on msg/s. A 20% regression would ship. |
| **No property-based test for sublist match** (e.g., random wildcards vs random subjects) | **Low** | Would catch off-by-one in `>` matching. |

**Concrete additions to hit parity:**
1. `tests/test_fuzz_parser.c` — libFuzzer harness, 50 LOC, wired in CMake behind `CMQ_ENABLE_FUZZ`.
2. `tests/test_tls_handshake.c` — OpenSSL client/server BIO pair, verify AEAD suites selected.
3. `tests/test_recovery_e2e.c` — `fork()`, child writes, parent restarts, verify replay.
4. `tests/test_mqtt_broker.c` — full CONNECT/PUBLISH/DISCONNECT round-trip.
5. `bench/ci_bench.sh` — fail CI if e2e drops below 25K msg/s (current baseline −20%).

---

## 9. Production-Readiness Score

**Tier 1 (must-fix before production):** 
- **F11 (wire flag rejection)** — SHIPPED v0.2.0 ✅
- **F7 (build hardening)** — SHIPPED v0.2.0 ✅
- **F8 (auth password plaintext)** — **DEFERRED, blocks production.** v0.2.0 ships plaintext `strdup` (`cmq_config.c:124-125`). CWE-256. Needs libsodium + Argon2id migration (§4 of this report).
- **F1 (TLS)** — SHIPPED v0.2.0 ✅ but lacks test coverage.

**Tier 2 (should-fix in v0.3.x):**
- **F5 (persistence recovery end-to-end)** — best-effort + O(N) replay. No snapshot, no rotation.
- **F9 (CRC32C hardware)** — SHIPPED ✅, software fallback tested.
- **F10 (rate limit)** — SHIPPED ✅ but no bandwidth quota.
- **F12/F13 (ops endpoints)** — SHIPPED ✅.
- **Cluster/leaf/gateway tests** — code present, test coverage absent.

**Tier 3 (production-grade aspirations, out of scope):**
- **JetStream / KV / Object Store** — months of work; define the v1.x scope.
- **EOS, transactions, tiered storage** — Kafka-class features.
- **NKEY/JWT auth** — replaces F8's libsodium path with full decentralized identity.
- **Performance: 100K–1M msg/s** — needs sharded sublist + io_uring.
- **Log compaction** — needs key-aware persistence.

**Bottom line:** v0.2.0 closes the interop bug, ships TLS, persistence, hardening, rate-limit, and ops endpoints. It is **NOT** production-ready because **F8 is deferred**. After F8 lands, it's Tier-1 production-grade for a small single-tenant deployment behind a firewall. Full production grade (multi-tenant, cluster-wide, durable, observable at scale) needs Tier-2 + Tier-3 work, estimated 6-12 months for one engineer.

---

## 10. Source Citations

**NATS.**
- https://docs.nats.io/running-a-nats-service/configuration/accounts
- https://docs.nats.io/running-a-nats-service/configuration/securing_nats/auth_intro/jwt
- https://docs.nats.io/running-a-nats-service/clustering
- https://docs.nats.io/running-a-nats-service/clustering/gateways
- https://docs.nats.io/nats-concepts/jetstream
- https://docs.nats.io/using-nats/nats-tools/nats_cli/natsbench
- https://github.com/nats-io/nats-server
- https://gist.github.com/derekcollison/4616831fc476e5f2117e

**Kafka.**
- https://kafka.apache.org/documentation/#log_compaction
- https://kafka.apache.org/documentation/#semantics
- https://kafka.apache.org/documentation/#design_quotas
- https://kafka.apache.org/documentation/#tiered_storage
- https://kafka.apache.org/documentation/#transactions
- https://docs.confluent.io/platform/current/kafka/design/log_compaction.html
- https://engineering.linkedin.com/kafka/benchmarking-apache-kafka-2-million-writes-second-three-cheap-machines

**MQTT brokers.**
- https://github.com/eclipse/mosquitto (EPL/EDL, NOT MIT)
- https://github.com/emqx/nanomq (MIT — best reference for CMSGQueue)
- https://github.com/cesanta/mongoose (GPLv2)
- https://github.com/wolfSSL/wolfMQTT (GPLv2 OR commercial)

**Password hashing.**
- https://raw.githubusercontent.com/OWASP/CheatSheetSeries/master/cheatsheets/Password_Storage_Cheat_Sheet.md (authoritative)
- https://datatracker.ietf.org/doc/html/rfc9106 (Argon2)
- https://datatracker.ietf.org/doc/html/rfc7914 (scrypt)
- https://datatracker.ietf.org/doc/html/rfc2898 (PBKDF2)
- https://doc.libsodium.org/password_hashing/default_phf (libsodium)

**TLS.**
- https://wiki.mozilla.org/Security/Server_Side_TLS (Mozilla intermediate profile)
- https://datatracker.ietf.org/doc/html/rfc8446 (TLS 1.3)
- https://datatracker.ietf.org/doc/html/rfc5246 (TLS 1.2 reference)
- https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_status_cb/
- https://docs.openssl.org/3.0/man3/SSL_CTX_set1_curves_list/

**WAL / recovery.**
- https://raw.githubusercontent.com/google/leveldb/main/doc/log_format.md (LevelDB log format)
- https://github.com/google/leveldb/blob/main/doc/index.md
- https://www.postgresql.org/docs/current/wal-intro.html
- https://www.postgresql.org/docs/current/wal-configuration.html

**Performance benchmarks.**
- https://gist.github.com/derekcollison/4616831fc476e5f2117e (NATS 14M msg/s)
- https://docs.nats.io/using-nats/nats-tools/nats_cli/natsbench
- https://devopedia.org/redis-streams
- https://github.com/confluentinc/throughput-latency-benchmark (Kafka)

**Internal CMSGQueue evidence.**
- `src/server/cmq_config.c:124-125` (plaintext auth_password)
- `src/store/cmq_filestore.c:54-65` (64-byte fixed records, software CRC32C)
- `src/cluster/cmq_leaf.c:1366` LOC, `cmq_gateway.c:885` LOC (no tests)
- `src/proto/cmq_parser.c:60-67` (max payload 2× cap pre-F15)
- `tests/test_cluster.c:332` (no leaf/gateway tests)
- `docs/features/persistence.md` (recovery is synchronous, best-effort)
- `docs/benchmarks/results.md:26` (~30K msg/s end-to-end)
- `CHANGELOG.md:32` (F8 deferred; libsodium header blocker)