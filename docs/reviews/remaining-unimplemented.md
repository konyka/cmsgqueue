# Remaining unimplemented work (HEAD after v0.5.60)

Evidence-checked against source on 2026-09-06. P2 (R1–R7) and
P3 D7/D8 are shipped. D4 library pieces are v0.5.56–59. D5
phases 1–2 are v0.5.55/60. Next cuts: D1–D3, D5 multi-node
2PC, or leaf/gateway e2e.

## Shipped (do not re-open)

| Ver | Item |
|---|---|
| v0.5.41–47 | F2 BATCH compress, aux accept, MQTT 5 props, trace logs, WAL compact/rotate, MQTT will/sessions, connz/subz/routez |
| v0.5.48 | D8 concurrent account conn/sub/payload caps |
| v0.5.49 | D7 publish-side subject rewrite |
| v0.5.50 | F14 connect-rate on CONNECT |
| v0.5.51 | Audit auth / persist / TLS events |
| v0.5.52 | Per-account outstanding-byte (`bytes_live`) cap |
| v0.5.53 | D6 Kafka-style key compact on sealed `.1` |
| v0.5.54 | MQTT QoS 1 outbound inflight + local fanout |
| v0.5.55 | D5 phase 1: idempotent publish pid+seq window |
| v0.5.56 | D4 phase 1: durable stream consumer cursors |
| v0.5.57 | MQTT outbound QoS 2 PUBREC/PUBREL/PUBCOMP |
| v0.5.58 | D4 phase 2: KV last-value store |
| v0.5.59 | D4 phase 3: named object store |
| v0.5.60 | D5 phase 2: transaction coordinator |

## Deferred — detailed designs

### D1 OpenTelemetry exporter (XL)

NATS `trace` / OTel spans on publish and consume. No OTLP client
in tree. **Design (when we take it):** allocation-free span IDs
already exist (`trace_hex`). Export is a **sidecar thread** +
bounded lock-free ring (drop newest on overflow). Never on the
PUBLISH hot path. gRPC/HTTP exporter behind a compile flag.
Security: no payload bodies in spans by default.

### D2 HTTP/2 listener (L)

`cmq_tls_set_alpn` can encode `h2`, but the server never calls
it. No h2 state machine, no HPACK. **Design:** keep HTTP/1.1
monitor on a dedicated listener. Do not put h2 on the message
port. HPACK table capped (4 KiB); max 32 streams. Do not
advertise `h2` until that exists.

### D3 JWT / NKEY (XL)

No Ed25519/JWT in `src/`. **Design:** verify-only (no issue) using
the existing TLS/OpenSSL dependency. Cached JWKS, nkey public
keys in config. CONNECT presents a token; verify off the accept
hot path (worker). Reject on clock skew / issuer mismatch.
Do not add a second crypto library.

### D4 JetStream / KV / Object Store — library shipped v0.5.56–59

Durable cursors, KV last-value, and named objects are
library APIs. **Remaining:** a partition / bucket server
path that wires them into PUBLISH.

### D5 Exactly-once / transactions — phases 1–2 shipped

Idempotent publish is v0.5.55. The coordinator (`CMQT`
begin/add/commit/abort + `{persist_dir}/cmq.txn`) is
v0.5.60. **Remaining:** multi-node 2PC / prepare across
routes.

### D6 Kafka-style key compaction — shipped v0.5.53

Sealed `.1` last-value-per-key + tombstone drop. Live append
unchanged. Tombstone TTL / dirty-ratio auto-trigger remain
optional follow-ups.

### Other known gaps (not P3 IDs)

| Item | Evidence | Next cut |
|---|---|---|
| MQTT outbound QoS 2 | shipped v0.5.57 | — |
| ALPN `h2` | Comment example only; server never calls `set_alpn` | Ship D2 or leave unset |
| Leaf/gateway e2e | Library exists, no multi-process test | Test-only increment |

## TDD rule for every increment

1. Red tests for the new contract.
2. Green on the smallest production path.
3. Docs + benches + CHANGELOG + push.
