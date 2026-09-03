# WS permessage-deflate (RFC 7692)

## Motivation

WebSocket clients (browsers, mobile, IoT gateways) subscribe to pub/sub
notifications that are typically repetitive JSON. Without compression,
every byte crosses the network twice (inbound publish → outbound WS frame).
RFC 7692 permessage-deflate typically achieves 60–80 % compression on JSON
and protobuf payloads, cutting both egress bandwidth and serialization CPU.

## Design

Server negotiates the extension in the WS upgrade handshake, then compresses
every outgoing TEXT / BINARY frame and decompresses every incoming one. Each
connection owns its own zlib stream (no shared dictionary) for state isolation.

### Negotiation

Server replies to a client `Sec-WebSocket-Extensions: permessage-deflate; ...`
request with:

```
Sec-WebSocket-Extensions: permessage-deflate;
    server_no_context_takeover; client_no_context_takeover
```

Both no-context-takeover flags are set so each connection gets a fresh
inflater / deflater per message. This trades ~256 KB of memory per idle
connection for tenant isolation (no information leak across connections via
shared zlib dictionary).

Unsupported parameters (e.g., `server_max_window_bits=12`) cause a parse
error → server sends HTTP 400. The negotiated window is always 15 bits (the
maximum, matching the spec default).

### Per-message boundary

Each message is deflated with `Z_SYNC_FLUSH` and the trailing magic
`0x00 0x00 0xFF 0xFF` is appended. The receiver feeds the byte sequence
to `inflate()` with `windowBits = -15` (raw deflate, no zlib header) and
recover the original payload bytes.

### Safety

| Risk | Mitigation |
|---|---|
| Decompression bomb (1 KB → 4 GB) | Hard 16 MB frame cap from `cmq_ws_frame_parse` applies to the deflated payload; the inflate output inherits the same cap. |
| Z_DATA_ERROR on corrupt stream | Returns `-1`; caller tears down the connection. |
| Memory leak on connection close | `inflateEnd` / `deflateEnd` called in destroy path. |
| Cross-tenant information leak | Per-connection z_stream, no shared dictionary. |

### Reliability

- `Z_SYNC_FLUSH` produces a clean receiver sync point.
- Per-message boundary is self-describing (the trailing 0x00 0x00 0xFF 0xFF).
- Concurrency: each connection has its own z_stream; no locks needed.

## API

```c
int cmq_ws_parse_extensions(const char *req, size_t req_len);
int cmq_ws_build_extensions_response(char *out, size_t out_len);
int cmq_ws_deflate_message(const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t out_cap);
int cmq_ws_inflate_message(const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t out_cap);
```

## Files

- `src/enterprise/cmq_ws.h` — declarations.
- `src/enterprise/cmq_ws.c` — implementation (zlib integration).
- `tests/test_ws_deflate.c` — roundtrip + negotiation test (8 cases).
- `CMakeLists.txt` — links `ZLIB::ZLIB` when found.

## Bench impact

Compression is per-message and runs only on the WS path. Core TCP/NATS
benchmark is unchanged: ~33K msg/s, p99 99 µs (same as v0.5.20).
