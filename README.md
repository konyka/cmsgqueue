# CMSGQueue

High-performance message queue server in pure C (C11). Custom binary protocol with 0xCA 0xFE magic, assembly-based coroutines, memory pools, and full feature parity with NATS Server.

## Features

- **Pub/Sub** with subject-based routing and wildcard matching (`*`, `>`)
- **Queue Groups** with round-robin delivery within shared subscription groups
- **Request-Reply** pattern via REQUEST/RESPONSE ops with reply-to subjects
- **Message Headers** passthrough (key-value pairs on PUBLISH/MESSAGE frames)
- **Batch Publish** send multiple messages in a single frame (CMQ_OP_BATCH)
- **Server Stats** query connections, messages, bytes, subscriptions via CMQ_OP_STATS
- **Authentication** username/password on CONNECT, configurable via config file
- **Keepalive** automatic disconnect of idle clients (configurable ping_interval_ms)
- **Backpressure** 4MB write buffer limit per client; validated by tests/test_parser_backpressure.c
- **Connection Limit** atomic `active_clients` gate on accept (`max_clients`)
- **Graceful Shutdown** cmq_server_drain() sends DISCONNECT to all clients before stopping; validated by tests/test_server_ops.c shutdown path
- **Persistence** ring buffer memstore, durable streams with consumers, file-based with CRC32 (library APIs; optional server wiring)
  Note: durable streams and file-based persistence are library APIs only — not wired into the server process.
- **Clustering** node membership and outbound route broadcast (gateway/leaf APIs available as libraries)
- **Enterprise** account counters, TLS accept stub (plaintext until OpenSSL wired, F1 pending), MQTT bridge library with **5.0 property decode (v0.5.43)** and **will / durable sessions (v0.5.46)**, WebSocket transport with frame reassembly
- **Connection tracing (v0.5.44)**: 16-byte ID at accept, `[tid=hex]` on `cmq_log` lines for that connection
- **WAL compact / rotate (v0.5.45)**: `cmq_filestore_compact` keeps a tail; `set_rotate_bytes` archives to `.1` so the live WAL stays bounded
- **HTTP introspection (v0.5.47)**: `/connz` `/subz` `/routez` JSON snapshots beside `/healthz` `/readyz` `/metrics`
- **Per-account limits (v0.5.48)**: concurrent conn/sub + per-message payload caps (`account_max_*`; `0` = unlimited)
- **Subject rewrite (v0.5.49)**: per-account publish maps (`foo.*` → `bar.$1`); skipped when `map_total` is 0
- **Connect-rate (v0.5.50)**: `max_connections_per_account` is live on CONNECT (per-second window; distinct from concurrent `account_max_connections`)
- **Audit trail (v0.5.51)**: `auth_ok` / `auth_fail` / `persist_*` / `tls_handshake_fail` on their real paths (not only `rate_limit_reject`)
- **Outstanding-byte cap (v0.5.52)**: `account_max_bytes_live` bounds concurrent in-flight ingress per account (`0` = unlimited)
- **Key compaction (v0.5.53)**: `cmq_filestore_compact_keys` last-value-wins on sealed `.1` segments (live append unchanged)
- **MQTT QoS 1 inflight (v0.5.54)**: 16-slot outbound window; local fanout + PUBACK (SUBSCRIBE still grants at most QoS 1)
- **Idempotent publish (v0.5.55)**: `CMQI`+pid+seq sliding window drops retries before WAL (D5 phase 1)
- **Durable stream cursors (v0.5.56)**: opt-in `{dir}/{name}.cursors` ack watermarks (D4 phase 1; append path unchanged)
- **MQTT QoS 2 outbound (v0.5.57)**: grant 2; PUBLISH/PUBREC/PUBREL/PUBCOMP on the 16-slot window
- **KV store (v0.5.58–70)**: last-value put/get/del; `$KV.<bucket>.<key>` PUBLISH + REQUEST-get
- **Object store (v0.5.59–70)**: named blobs; `$OBJ.<name>` PUBLISH + REQUEST-get when persist_dir is set
- **Transactions (v0.5.60 / v0.5.80)**: `CMQT` begin/add/commit/abort + 2PC PREPARE/VOTE across live routes (D5 phases 2–3)
- **OTel / OTLP (v0.5.61–64, 0.5.78)**: lock-free 256-slot sidecar; OTLP/HTTP(S) JSON POST when `otlp_endpoint` is set (D1 phases 1–3)
- **JWT / NKEY / JWKS (v0.5.62–65, 0.5.74–79, 0.5.82)**: HS256 / ES256 / RS256 JWT on CONNECT; Ed25519 nkey of `CMQNK1|<user>` (`nkey_pub` is 64 hex or NATS `U…`); JWKS oct/EC/RSA `kid` cache + HTTP/HTTPS `jwks_url` + `jwks_refresh_sec` (D3 phases 1–9)
- **HTTP/2 (v0.5.66–73, 0.5.81)**: HPACK static + Huffman + 4 KiB dynamic table + preface/SETTINGS/32-stream machine + loopback listener + `h2_port` / ALPN `h2` (D2 phases 1–6)
- **Build Hardening (F7)**: FORTIFY_SOURCE=2, PIE, RELRO, stack-protector-strong (with hot-path exclusions for cmq_parser.c, cmq_slab.c, cmq_mpool.c)
- **Hardware CRC32C (F9)**: SSE4.2 / aarch64 CRC32 hardware acceleration with software fallback
- **Wire Checksum (F3)**: CMQ_FLAG_CHECKSUM with CRC32C trailing 4 bytes; rejects bit-flips with 1 - 2⁻³² probability
- **Capability Negotiation (F4)**: extended INFO frame advertises server_id, max_payload, auth, tls, compression, checksum, headers, batch
- **Wire flags**: CMQ_FLAG_HEADERS, CMQ_FLAG_BATCH, CMQ_FLAG_ROUTE are implemented; **CMQ_FLAG_CHECKSUM is now implemented (F3)** with CRC32C verification (RFC 3309 / SSE4.2 HW-accelerated); **CMQ_FLAG_COMPRESSED is implemented for BATCH only (F2, v0.5.41)** — zstd, dest size from the frame content-size header, 16 MiB bomb cap. COMPRESSED on PUBLISH/MESSAGE is still rejected (F11 interop).
- **v0.5.0 hot path**:
  - **F1** test_stress flake fix: subscribe-publish barrier + deterministic drain.
  - **F2** audit log rotation at 100 MiB (`cmq-audit.log` → `cmq-audit.log.1`).
  - **F3** N1 per-subject rate limit enforced in `handle_publish`.
  - **F4** N2 hot config reload (`cmq_server_reload` re-reads blocklist/audit/log levels).
  - **F5** F14/F15/F16 wire-up: blocklist in `accept_cb`, ACL + quota in `handle_publish`.
  - **F6** N3 audit log file creation test.
  - **F7** mTLS API surface tests.
  - **F8** F17 BIO-wrap write_full/read wiring (`cmq_route_tls_sess_t` integration; full socket BIO-wrap in v0.5.1).
  - **F9** F18 wire-up: subscriptions persisted on sub/unsub.
  - **F10** F19 server-side MQTT listener tests (full state machine deferred to v0.5.3).
- **Performance**: 33,784 msg/s end-to-end, 30 µs avg latency (v0.5.0 baseline; preserved by v0.5.0).
- **Assembly Coroutines** x86_64 + ARM64 context switching; high-fanout delivery uses value snapshots (no live ref UAF)
- **Multi-Worker** N worker threads with eventfd cross-thread messaging keyed by stable client id
- **Cross-Platform CI** Linux (gcc/clang), macOS, Windows, ARM64 cross-compile

## Quick Start

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Run Tests

```bash
cd build
ctest --output-on-failure -j$(nproc)
```

### Run Server

```bash
./build/examples/pubsub both 0.0.0.0 7654
```

### Configuration

Create `cmq.conf`:

```ini
# Network
host = 0.0.0.0
port = 7654
threads = 4
max_clients = 10000

# Logging
log_level = info
log_to_stdout = 1

# Authentication
auth_username = admin
auth_password = secret

# Limits
max_payload_size = 1048576
```

## Binary Protocol

Custom binary protocol with 9-byte packed header (client-speaks-first):

```
Offset  Size  Field
0       2     Magic (0xCA 0xFE)
2       1     Version (0x01)
3       1     Flags
4       1     Opcode
5       4     Payload length (little-endian uint32)
9       ...   Payload
```

### Opcodes

| Opcode | Name | Direction | Description |
|--------|------|-----------|-------------|
| 0x01 | CONNECT | C→S | Client handshake |
| 0x02 | CONNACK | S→C | Connection acknowledgment |
| 0x03 | PUBLISH | C→S | Publish message |
| 0x04 | PUBACK | S→C | Publish acknowledgment |
| 0x05 | SUBSCRIBE | C→S | Subscribe to subject |
| 0x06 | SUBACK | S→C | Subscription acknowledgment |
| 0x07 | UNSUBSCRIBE | C→S | Unsubscribe |
| 0x08 | UNSUBACK | S→C | Unsubscribe acknowledgment |
| 0x09 | MESSAGE | S→C | Deliver message to subscriber |
| 0x0A | PING | C→S | Keep-alive |
| 0x0B | PONG | S→C | Keep-alive response |
| 0x0C | DISCONNECT | Either | Graceful disconnect |
| 0x0D | ERROR | S→C | Error response |
| 0x0E | INFO | S→C | Server info (sent after first client frame) |
| 0x0F | REQUEST | C→S | Request with reply-to subject |
| 0x10 | RESPONSE | C→S | Response to a request |
| 0x11 | STATS | C→S / S→C | Query server metrics |
| 0x12 | BATCH | C→S | Batch publish (multiple messages) |

### Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0x01 | CMQ_FLAG_COMPRESSED | Payload is compressed |
| 0x02 | CMQ_FLAG_CHECKSUM | Frame carries a checksum |
| 0x04 | CMQ_FLAG_HEADERS | Headers block present |
| 0x08 | CMQ_FLAG_BATCH | Batch frame |

## Architecture

```
Client ──TCP──→  Acceptor (main thread)
Client ──TCP──→       │ dispatch (fd → worker)
Client ──TCP──→       ▼
                ┌─────────────────────────┐
                │   Worker Thread Pool (N)  │
                │  ┌───────────────────┐   │
                │  │  Event Loop       │   │
                │  │  (epoll/kqueue)   │   │
                │  │    ↕ coroutine    │   │
                │  │  ┌────────────┐   │   │
                │  │  │ Protocol   │   │   │
                │  │  │ Parser     │   │   │
                │  │  └────────────┘   │   │
                │  └───────────────────┘   │
                └─────────────────────────┘
                         │
                ┌────────┴────────┐
                │   Core Engine    │
                │  Sublist Trie    │
                │  Message Router  │
                │  Queue Groups    │
                │  Accounts        │
                └─────────────────┘
                         │
         ┌───────────────┼───────────────┐
         │               │               │
  ┌──────┴──────┐ ┌──────┴──────┐ ┌──────┴──────┐
  │ Persistence │ │  Clustering  │ │  Enterprise  │
  │ Memstore    │ │  Routes      │ │  Accounts    │
  │ Streams     │ │  Gateway     │ │  TLS         │
  │ Filestore   │ │  Leaf Nodes  │ │  MQTT Bridge │
  └─────────────┘ └─────────────┘ │  WebSocket   │
                                  └─────────────┘
```

## Module Reference

| Module | Files | Description |
|--------|-------|-------------|
| Memory Pool | `src/core/cmq_mpool.c`, `cmq_slab.c` | Arena + slab allocator |
| Logger | `src/core/cmq_log.c` | Async multi-appender logger |
| Coroutines | `src/coro/cmq_coro.c` | Assembly context switch (x86_64, ARM64) |
| Event Loop | `src/net/cmq_ev.c` | epoll/kqueue with eventfd wakeup |
| Protocol | `src/proto/cmq_parser.c` | Binary frame parser + encoder |
| Server | `src/server/cmq_server.c` | TCP accept, client lifecycle, routing |
| Sublist | `src/server/cmq_sublist.c` | Subject trie with `*` and `>` wildcards |
| Config | `src/server/cmq_config.c` | INI-style config parser |
| Store | `src/store/cmq_store.c` | Ring buffer memstore |
| Stream | `src/store/cmq_stream.c` | Durable log with consumers |
| Filestore | `src/store/cmq_filestore.c` | Append-only persistence with CRC32 |
| Cluster | `src/cluster/cmq_cluster.c` | Node membership + heartbeat |
| Route | `src/cluster/cmq_route.c` | Server-to-server forwarding |
| Gateway | `src/cluster/cmq_gateway.c` | Cross-cluster communication |
| Leaf | `src/cluster/cmq_leaf.c` | Lightweight edge connections |
| Account | `src/enterprise/cmq_account.c` | Multi-tenant isolation |
| TLS | `src/enterprise/cmq_tls.c` | TLS config + session lifecycle |
| MQTT | `src/enterprise/cmq_mqtt.c` | MQTT bridge + topic mapping |
| WebSocket | `src/enterprise/cmq_ws.c` | RFC 6455 frame parser + handshake |

## Examples

| Example | Description |
|---------|-------------|
| `examples/pubsub.c` | Server, publisher, and subscriber modes |
| `examples/streaming.c` | Memstore, stream with consumers, file persistence |
| `examples/cluster.c` | Cluster membership, routing, gateway, leaf nodes |
| `examples/request_reply.c` | Request-reply pattern with service, client, and stats query |
| `examples/benchmark.c` | Throughput benchmark (configurable clients, messages, threads) |

Build examples:
```bash
cmake --build build --target pubsub streaming cluster
```

## Test Suite

22 test targets with 236 registered test cases:

| Suite | Tests | Area |
|-------|-------|------|
| test_atomic | 4 | Platform atomics |
| test_mpool | 9 | Memory pool |
| test_slab | 8 | Slab allocator |
| test_log | 5 | Logger |
| test_coro | 6 | Coroutines |
| test_ev | 11 | Event loop |
| test_parser | 21 | Protocol parser |
| test_config | 28 | Config parser |
| test_platform | 3 | Platform detection |
| test_queue | 4 | MPSC queue |
| test_sublist | 18 | Subject trie |
| test_server | 4 | TCP server integration |
| test_phase2 | 5 | Queue groups, auth, headers, stats |
| test_store | 22 | Persistence layer |
| test_cluster | 18 | Clustering |
| test_enterprise | 31 | Accounts, TLS, MQTT, WebSocket |
| test_integration | 4 | Account stats, WS detection |
| test_worker | 2 | Multi-worker pub/sub |
| test_stress | 3 | High-load: many clients, fan-out, wildcards |
| test_coro_integration | 2 | Coroutine high-fanout delivery |
| test_request_reply | 2 | Request-reply pattern |
| test_server_ops | 26 | Stats, batch, keepalive |

## Build Options

```bash
cmake .. \
  -DCMQ_BUILD_TESTS=ON \
  -DCMQ_BUILD_EXAMPLES=ON \
  -DCMQ_ENABLE_ASAN=ON \
  -DCMQ_ENABLE_UBSAN=ON \
  -DCMQ_ENABLE_COVERAGE=ON \
  -DCMQ_STATIC=ON
```

## CI

Cross-platform CI via GitHub Actions:
- **Linux GCC** (Debug + Release, ASan + UBSan)
- **Linux Clang** (Debug + Release, ASan)
- **macOS** (Debug + Release)
- **Windows** (Debug + Release)
- **ARM64** cross-compile (aarch64-linux-gnu)

## License

Apache License 2.0
