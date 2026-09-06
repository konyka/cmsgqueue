# Remaining unimplemented work (HEAD after v0.5.52)

Evidence-checked against source on 2026-09-06. P2 (R1–R7) and
P3 D7/D8 are shipped. F14 connect-rate is v0.5.50. Audit events
are v0.5.51. Outstanding-byte caps are v0.5.52. Next cut: D6.

## Shipped (do not re-open)

| Ver | Item |
|---|---|
| v0.5.41–47 | F2 BATCH compress, aux accept, MQTT 5 props, trace logs, WAL compact/rotate, MQTT will/sessions, connz/subz/routez |
| v0.5.48 | D8 concurrent account conn/sub/payload caps |
| v0.5.49 | D7 publish-side subject rewrite |
| v0.5.50 | F14 connect-rate on CONNECT |
| v0.5.51 | Audit auth / persist / TLS events |
| v0.5.52 | Per-account outstanding-byte (`bytes_live`) cap |

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

### D4 JetStream / KV / Object Store (XXL)

`cmq_stream` is a byte ring, not consumers. **Design (phased):**
(1) durable consumer cursor per stream, (2) KV as a 1-key
compacted subject, (3) object store last. Needs a partition
model we do not have. Not a single version.

### D5 Exactly-once / transactions (XL)

No PID+seq, no txn coordinator. **Design:** idempotent publish
first (client PID + seq in a header, server sliding window per
PID). Transactions need a coordinator log — after D4.

### D6 Kafka-style key compaction (L)

v0.5.45 compact is **tail retain**, not per-key. **Design:**
optional record key (first header `K`). Background cleaner
walks sealed `.1` segments only (never the live append path).
Keep last value per key; tombstone TTL. Trigger on dirty ratio.
Live PUBLISH path unchanged.

### Other known gaps (not P3 IDs)

| Item | Evidence | Next cut |
|---|---|---|
| MQTT QoS 1/2 inflight | Listener is QoS 0 + will | Inflight mqueue |
| ALPN `h2` | Comment example only; server never calls `set_alpn` | Ship D2 or leave unset |
| Leaf/gateway e2e | Library exists, no multi-process test | Test-only increment |

## TDD rule for every increment

1. Red tests for the new contract.
2. Green on the smallest production path.
3. Docs + benches + CHANGELOG + push.
