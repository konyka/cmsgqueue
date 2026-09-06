# Remaining unimplemented work (HEAD after v0.5.86)

Evidence-checked against source on 2026-09-06. P2 (R1–R7)
and P3 D1–D8 phase cuts in this catalog are shipped.
Required next cuts: none.

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
| v0.5.61 | D1 phase 1: OTel span ring + sidecar |
| v0.5.62 | D3 phase 1: JWT HS256 + Ed25519 nkey verify |
| v0.5.63 | D3 phase 2: nkey signature on CONNECT |
| v0.5.64 | D1 phase 2: OTLP/HTTP JSON exporter |
| v0.5.65 | D3 phase 3: JWKS oct-key cache |
| v0.5.66 | D2 phase 1: HPACK static codec |
| v0.5.67 | D4 phase 4: KV bucket PUBLISH path |
| v0.5.68 | D4 phase 5: object-store PUBLISH path |
| v0.5.69 | D2 phase 2: HTTP/2 frame state machine |
| v0.5.70 | D4 phase 6: KV/object REQUEST-get |
| v0.5.71 | D2 phase 3: HPACK Huffman |
| v0.5.72 | D2 phase 4: HPACK 4 KiB dynamic table |
| v0.5.73 | D2 phase 5: HTTP/2 dedicated listener |
| v0.5.74 | D3 phase 4: JWT ES256 + JWKS EC |
| v0.5.75 | D3 phase 5: nkey seed / base32 |
| v0.5.76 | D3 phase 6: remote JWKS HTTP GET |
| v0.5.77 | D3 phase 7: JWT RS256 + JWKS RSA |
| v0.5.78 | D1 phase 3: OTLP HTTPS POST |
| v0.5.79 | D3 phase 8: HTTPS JWKS GET |
| v0.5.80 | D5 phase 3: multi-node 2PC |
| v0.5.81 | D2 phase 6: ALPN h2 + h2_port |
| v0.5.82 | D3 phase 9: JWKS refresh |
| v0.5.83 | D2 phase 7: TLS-wrapped h2 I/O |
| v0.5.84 | D1 phase 4: OTLP/gRPC |
| v0.5.85 | D5 phase 4: route write retry |
| v0.5.86 | Leaf/gateway CONNECT/CONNACK e2e |

## Deferred — detailed designs

### D1 OpenTelemetry exporter — phases 1–4 shipped v0.5.61–64, 0.5.78, 0.5.84

Span ring, sidecar, OTLP/HTTP JSON, OTLP HTTPS POST, and
OTLP/gRPC (`grpc://`, protobuf Export over HTTP/2) are
live. Consume spans are queued only when a caller offers
`KIND_CONSUME`.

### D2 HTTP/2 listener — phases 1–7 shipped v0.5.66–73, 0.5.81, 0.5.83

HPACK static codec, Huffman, the 4 KiB dynamic table, the
preface/SETTINGS/32-stream machine, a loopback
prior-knowledge listener, `h2_port` bind, TLS ALPN `h2`,
and TLS-wrapped accept (`cmq_h2_accept_tls`) are live.

### D3 JWT / NKEY / JWKS — phases 1–9 shipped v0.5.62–65, 0.5.74–79, 0.5.82

HS256 JWT, ES256 (P-256), RS256 (2048–4096), Ed25519 nkey
on CONNECT (`U…` or 64 hex), static JWKS (oct/EC/RSA),
HTTP/HTTPS `jwks_url` fetch, and periodic refresh
(`jwks_refresh_sec`) are live. Still verify-only.

### D4 JetStream / KV / Object Store — phases 1–6 shipped

Durable cursors, KV last-value, named objects,
`$KV.<bucket>.<key>` / `$OBJ.<name>` PUBLISH and REQUEST-get
are live. Partitioned consume cursors beyond the library API
remain optional follow-ups.

### D5 Exactly-once / transactions — phases 1–4 shipped

Idempotent publish is v0.5.55. The coordinator (`CMQT`
begin/add/commit/abort + `{persist_dir}/cmq.txn`) is
v0.5.60. Multi-node 2PC (PREPARE / VOTE / COMMIT across
live routes, 200 ms) is v0.5.80. EAGAIN route writes
retry from a 32-slot queue (v0.5.85).

### D6 Kafka-style key compaction — shipped v0.5.53

Sealed `.1` last-value-per-key + tombstone drop. Live append
unchanged. Tombstone TTL / dirty-ratio auto-trigger remain
optional follow-ups.

### Other known gaps (not P3 IDs)

| Item | Evidence | Next cut |
|---|---|---|
| MQTT outbound QoS 2 | shipped v0.5.57 | — |
| ALPN `h2` | shipped v0.5.81 / TLS wrap v0.5.83 | — |
| Leaf/gateway e2e | shipped v0.5.86 (`test_leafe.c`) | — |

## Optional follow-ups (not required next cuts)

| Item | Notes |
|---|---|
| D4 partitioned consume cursors | Library cursors exist; no partitioned consumer API |
| D6 tombstone TTL / dirty-ratio auto-trigger | Compact is explicit; no TTL sidecar |
| D3 token issuing | Intentionally verify-only |
| D1 consume spans | Only when caller offers `KIND_CONSUME` |

## TDD rule for every increment

1. Red tests for the new contract.
2. Green on the smallest production path.
3. Docs + benches + CHANGELOG + push.
