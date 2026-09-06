# Remaining unimplemented work (HEAD after v0.5.126)

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
| v0.5.87 | D4 partitioned consume cursors |
| v0.5.88 | D6 tombstone TTL + dirty-ratio compact |
| v0.5.89 | D1 consume spans |
| v0.5.90 | D3 JWT HS256 issuing |
| v0.5.91 | D3 JWT ES256 / RS256 issuing |
| v0.5.92 | D1 connect spans |
| v0.5.93 | D4 `$JS.<name>` stream PUBLISH path |
| v0.5.94 | D4 `$JS.<name>` REQUEST-get |
| v0.5.95 | D4 `$JS.<name>.<consumer>` consume / ack |
| v0.5.96 | F2 COMPRESSED on PUBLISH |
| v0.5.97 | F2 COMPRESSED on MESSAGE |
| v0.5.98 | F2 COMPRESSED on REQUEST |
| v0.5.99 | F2 COMPRESSED on RESPONSE |
| v0.5.100 | D1 request spans |
| v0.5.101 | D1 response spans |
| v0.5.102 | D1 disconnect spans |
| v0.5.103 | D4 durable `$JS` last payload |
| v0.5.104 | D4 durable `$JS` history WAL |
| v0.5.105 | D4 `$JS` hash partitions |
| v0.5.106 | D4 `$JS` consume-part subject |
| v0.5.107 | D4 `$JS` default partitions |
| v0.5.108 | D4 `$JS` history WAL rotate |
| v0.5.109 | F5 `persist_dir` config-file key |
| v0.5.110 | P3 `persist_sync_interval_ms` config-file key |
| v0.5.111 | F6 MQTT bridge config-file keys |
| v0.5.112 | F6 MQTT bridge outbound PUBLISH |
| v0.5.113 | config string ownership (tls_ca / ACL / blocklist) |
| v0.5.114 | listener TLS config keys |
| v0.5.115 | extra-listener bind host/port |
| v0.5.116 | reload log_level and acl_deny |
| v0.5.117 | reload TLS cert/key |
| v0.5.118 | SIGHUP / config_file |
| v0.5.119 | reload auth / JWT / nkey |
| v0.5.120 | reload JWKS cache from jwks_json |
| v0.5.121 | reload persist_sync_interval_ms |
| v0.5.122 | reload live rate / timeout scalars |
| v0.5.123 | reload payload / sub / client caps |
| v0.5.124 | reload F14 quota / N1 subject RL |
| v0.5.125 | reload account_max_* defaults |
| v0.5.126 | reload MQTT bridge maps |

## Deferred — detailed designs

### D1 OpenTelemetry exporter — phases 1–4 shipped v0.5.61–64, 0.5.78, 0.5.84

Span ring, sidecar, OTLP/HTTP JSON, OTLP HTTPS POST, and
OTLP/gRPC (`grpc://`, protobuf Export over HTTP/2) are
live. A successful local fanout offers one `KIND_CONSUME`
with the publisher trace (v0.5.89). CONNACK 0 offers
`KIND_CONNECT` (v0.5.92). A successful local REQUEST
answer offers `KIND_REQUEST` (v0.5.100). A successful
local RESPONSE deliver offers `KIND_RESPONSE` (v0.5.101).
A graceful inbound DISCONNECT offers `KIND_DISCONNECT`
(v0.5.102).

### D2 HTTP/2 listener — phases 1–7 shipped v0.5.66–73, 0.5.81, 0.5.83

HPACK static codec, Huffman, the 4 KiB dynamic table, the
preface/SETTINGS/32-stream machine, a loopback
prior-knowledge listener, `h2_port` bind, TLS ALPN `h2`,
and TLS-wrapped accept (`cmq_h2_accept_tls`) are live.

### D3 JWT / NKEY / JWKS — phases 1–9 shipped v0.5.62–65, 0.5.74–79, 0.5.82

HS256 JWT, ES256 (P-256), RS256 (2048–4096), Ed25519 nkey
on CONNECT (`U…` or 64 hex), static JWKS (oct/EC/RSA),
HTTP/HTTPS `jwks_url` fetch, and periodic refresh
(`jwks_refresh_sec`) are live. Mint: `cmq_jwt_sign_hs256`,
`cmq_jwt_sign_es256` (P-256 `d`), `cmq_jwt_sign_rs256`
(`n`/`e`/`d`).

### D4 JetStream / KV / Object Store — phases 1–6 shipped

Durable cursors, KV last-value, named objects,
`$KV.<bucket>.<key>` / `$OBJ.<name>` / `$JS.<name>` PUBLISH
and REQUEST-get, `$JS.<name>.<consumer>` pull consume / ack,
and partitioned consume cursors (1–16, `append_key` /
`next_part` / `ack_part`, `CMQC2`) are live. The last
`$JS` payload is durable (`{persist_dir}/js/{name}.last`,
v0.5.103). History is a `CMQM` append WAL
(`{persist_dir}/js/{name}.msgs`, v0.5.104) replayed on
open. `$JS` hash partitions (`cmq_js_set_partitions`,
`{name}.parts` `CMQP`, payload `append_key`,
`consume_part`) are live (v0.5.105). REQUEST
`$JS.<name>.<consumer>.<part>` reaches `consume_part`
via `cmq_js_consume` (v0.5.106). New streams inherit
`js_partitions` / `cmq_js_set_default_partitions` when no
`.parts` file exists (v0.5.107). History WAL rotate
(`js_msgs_rotate_bytes`, v0.5.108) rewrites `.msgs` to a
bounded tail.

### D5 Exactly-once / transactions — phases 1–4 shipped

Idempotent publish is v0.5.55. The coordinator (`CMQT`
begin/add/commit/abort + `{persist_dir}/cmq.txn`) is
v0.5.60. Multi-node 2PC (PREPARE / VOTE / COMMIT across
live routes, 200 ms) is v0.5.80. EAGAIN route writes
retry from a 32-slot queue (v0.5.85).

### D6 Kafka-style key compaction — shipped v0.5.53

Sealed `.1` last-value-per-key + tombstone drop. Live append
unchanged. Tombstone TTL (segment mtime) and dirty-ratio
auto-compact (`set_compact_dirty`, after rotate / `maybe`)
are live (v0.5.88).

### Other known gaps (not P3 IDs)

| Item | Evidence | Next cut |
|---|---|---|
| MQTT outbound QoS 2 | shipped v0.5.57 | — |
| ALPN `h2` | shipped v0.5.81 / TLS wrap v0.5.83 | — |
| Leaf/gateway e2e | shipped v0.5.86 (`test_leafe.c`) | — |
| Bridge WAL recover | shipped v0.5.40 (`replay_one_record` CMQB) | — |
| `$JS.<name>` PUBLISH | shipped v0.5.93 | — |
| `$JS.<name>` REQUEST-get | shipped v0.5.94 | — |
| `$JS.<name>.<consumer>` consume / ack | shipped v0.5.95 | — |
| COMPRESSED on PUBLISH | shipped v0.5.96 | — |
| COMPRESSED on MESSAGE | shipped v0.5.97 | — |
| COMPRESSED on REQUEST | shipped v0.5.98 | — |
| COMPRESSED on RESPONSE | shipped v0.5.99 | — |
| REQUEST spans | shipped v0.5.100 | — |
| RESPONSE spans | shipped v0.5.101 | — |
| DISCONNECT spans | shipped v0.5.102 | — |
| `$JS` last payload persist | shipped v0.5.103 | — |
| `$JS` history WAL | shipped v0.5.104 | — |
| `$JS` hash partitions | shipped v0.5.105 | — |
| `$JS` consume-part subject | shipped v0.5.106 | — |
| `$JS` default partitions | shipped v0.5.107 | — |
| `$JS` history WAL rotate | shipped v0.5.108 | — |
| `persist_dir` config key | shipped v0.5.109 | — |
| `persist_sync_interval_ms` config key | shipped v0.5.110 | — |
| MQTT bridge config keys | shipped v0.5.111 | — |
| MQTT bridge outbound PUBLISH | shipped v0.5.112 | — |
| config string ownership | shipped v0.5.113 | — |
| listener TLS config keys | shipped v0.5.114 | — |
| extra-listener bind host/port | shipped v0.5.115 | — |
| reload log_level and acl_deny | shipped v0.5.116 | — |
| reload TLS cert/key | shipped v0.5.117 | — |
| SIGHUP / config_file | shipped v0.5.118 | — |
| reload auth / JWT / nkey | shipped v0.5.119 | — |
| reload JWKS cache from jwks_json | shipped v0.5.120 | — |
| reload persist_sync_interval_ms | shipped v0.5.121 | — |
| reload live rate / timeout scalars | shipped v0.5.122 | — |
| reload payload / sub / client caps | shipped v0.5.123 | — |
| reload F14 quota / N1 subject RL | shipped v0.5.124 | — |
| reload account_max_* defaults | shipped v0.5.125 | — |
| reload MQTT bridge maps | shipped v0.5.126 | — |
| COMPRESSED on control ops | SUBSCRIBE / CONNECT still rejected (intentional) | — |

## Optional follow-ups (not required next cuts)

- Slot 0 TLS stays the legacy `tls_*` keys. Extra-listener
  host/port shipped v0.5.115 (omit = `127.0.0.1:port+li`).
  `config_file` / SIGHUP shipped v0.5.118. Auth / JWT /
  nkey reload shipped v0.5.119. Static `jwks_json` cache
  reload shipped v0.5.120 (`jwks_url` refresh stays
  create-time). `persist_sync_interval_ms` reload shipped
  v0.5.121. Live rate / timeout scalars shipped v0.5.122.
  Payload / sub / client caps shipped v0.5.123. F14
  quota / N1 subject RL reload shipped v0.5.124.
  account_max_* defaults shipped v0.5.125. MQTT bridge
  map reload shipped v0.5.126 (addr/port stay create-time).

## TDD rule for every increment

1. Red tests for the new contract.
2. Green on the smallest production path.
3. Docs + benches + CHANGELOG + push.
