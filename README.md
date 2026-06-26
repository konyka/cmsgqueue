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
- **Backpressure** 4MB write buffer limit per client, slow consumers auto-disconnected
- **Connection Limit** configurable max_clients enforced on accept
- **Graceful Shutdown** cmq_server_drain() sends DISCONNECT to all clients before stopping
- **Persistence** ring buffer memstore, durable streams with consumers, file-based with CRC32
- **Clustering** node membership, server-to-server route forwarding, cross-cluster gateway, leaf nodes
- **Enterprise** multi-tenant accounts, TLS accept, MQTT bridge, WebSocket transport
- **Assembly Coroutines** x86_64 + ARM64 context switching, integrated for high-fanout delivery
- **Multi-Worker** N worker threads with eventfd-based cross-thread messaging, coroutine scheduler
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

Custom binary protocol with 9-byte packed header:

```
Offset  Size  Field
0       2     Magic (0xCA 0xFE)
2       1     Version (0x01)
3       1     Flags
4       1     Opcode
5       4     Payload length (big-endian uint32)
9       ...   Payload
```

### Opcodes

| Opcode | Name | Direction | Description |
|--------|------|-----------|-------------|
| 0x01 | INFO | S→C | Server info on connect |
| 0x02 | CONNECT | C→S | Client handshake |
| 0x03 | CONNACK | S→C | Connection acknowledgment |
| 0x04 | PUBLISH | C→S | Publish message |
| 0x05 | PUBACK | S→C | Publish acknowledgment |
| 0x06 | SUBSCRIBE | C→S | Subscribe to subject |
| 0x07 | SUBACK | S→C | Subscription acknowledgment |
| 0x08 | UNSUBSCRIBE | C→S | Unsubscribe |
| 0x09 | UNSUBACK | S→C | Unsubscribe acknowledgment |
| 0x0A | MESSAGE | S→C | Deliver message to subscriber |
| 0x0B | PING | C→S | Keep-alive |
| 0x0C | PONG | S→C | Keep-alive response |
| 0x0D | DISCONNECT | Either | Graceful disconnect |
| 0x0E | ERROR | S→C | Error response |
| 0x0F | REQUEST | C→S | Request with reply-to subject |
| 0x10 | RESPONSE | C→S | Response to a request |
| 0x11 | STATS | C→S / S→C | Query server metrics |
| 0x12 | BATCH | C→S | Batch publish (multiple messages) |

### Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0x01 | CMQ_FLAG_SUBJECT | Payload starts with subject string |
| 0x02 | CMQ_FLAG_REPLY | Reply-to subject present |
| 0x04 | CMQ_FLAG_HEADERS | Headers block present |

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

21 test suites with 200+ assertions:

| Suite | Tests | Area |
|-------|-------|------|
| test_atomic | 2 | Platform atomics |
| test_mpool | 4 | Memory pool |
| test_slab | 4 | Slab allocator |
| test_log | 5 | Logger |
| test_coro | 5 | Coroutines |
| test_ev | 4 | Event loop |
| test_parser | 11 | Protocol parser |
| test_config | 3 | Config parser |
| test_platform | 2 | Platform detection |
| test_store | 2 | Memstore |
| test_stream | 3 | Streams + consumers |
| test_filestore | 2 | File persistence |
| test_sublist | 5 | Subject trie |
| test_cluster | 2 | Cluster membership |
| test_enterprise | 5 | Accounts, TLS, MQTT, WS |
| test_server | 4 | Server lifecycle |
| test_phase2 | 4 | Queue groups, headers, auth, monitoring |
| test_integration | 4 | Account stats, WS detection |
| test_worker | 2 | Multi-worker pub/sub |
| test_stress | 3 | High-load: many clients, fan-out, wildcards |
| test_coro_integration | 2 | Coroutine high-fanout delivery |
| test_request_reply | 2 | Request-reply pattern |
| test_server_ops | 3 | Stats, batch, keepalive |

| Suite | Tests | Area |
|-------|-------|------|
| test_atomic | 2 | Platform atomics |
| test_mpool | 4 | Memory pool |
| test_slab | 4 | Slab allocator |
| test_log | 5 | Logger |
| test_coro | 5 | Coroutines |
| test_ev | 4 | Event loop |
| test_parser | 11 | Protocol parser |
| test_config | 10 | Config parser |
| test_platform | 3 | Platform detection |
| test_sublist | 14 | Subject trie |
| test_server | 4 | TCP server integration |
| test_phase2 | 5 | Queue groups, auth, headers, stats |
| test_store | 13 | Persistence layer |
| test_cluster | 12 | Clustering |
| test_enterprise | 23 | Accounts, TLS, MQTT, WebSocket |

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
