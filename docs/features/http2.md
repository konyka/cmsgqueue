# HTTP/2 (v0.5.66–73, 0.5.81, 0.5.83, D2 phases 1–7)

HPACK static + Huffman + 4 KiB dynamic table plus an
HTTP/2 connection state machine and a loopback listener.

## Frame layer (v0.5.69)

- Preface: `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` (24 bytes).
- 9-byte frame header. Payload cap 16 KiB.
- First frame after preface must be SETTINGS (not ACK).
- Max 32 concurrent streams. Client stream ids must be odd.
- A 33rd stream or a protocol error moves the conn to GOAWAY.

Huffman is v0.5.71. The 4 KiB dynamic table is v0.5.72. The
loopback prior-knowledge listener is v0.5.73 (`cmq_h2_listen`
/ `cmq_h2_session` / `cmq_h2_accept`). Bind is 127.0.0.1 only.
POST `:path` `/subject` plus DATA becomes subject + payload.

`h2_port` (v0.5.81) binds that listener on the server
(loopback only). When TLS is also enabled, the server calls
`cmq_tls_set_alpn("h2")` before `cmq_tls_load` so the CTX
advertises `h2`. `h2_port` 0 leaves both off.
v0.5.140: reload binds `h2_port` when create had none.
Omitted / 0 keeps the current fd. Out-of-range fails
closed. An existing listener is left alone (no accept-fd
rebind).

`cmq_h2_accept_tls` / `cmq_h2_session_tls` (v0.5.83) wrap
that same POST machine in a TLS handshake. Without a TLS
config, plaintext prior-knowledge accept is unchanged.
`cmq_server_h2_accept` uses TLS when slot 0 is configured.

## Tests

`tests/test_hpack.c`, `tests/test_huff.c`, `tests/test_hdyn.c`,
`tests/test_h2.c`, `tests/test_h2l.c`, `tests/test_h2p.c`,
`tests/test_h2t.c`, `tests/test_hup.c`

## See also

- `docs/reviews/v0.5.66.enumeration.md`
- `docs/reviews/v0.5.69.enumeration.md`
- `docs/reviews/v0.5.71.enumeration.md`
- `docs/reviews/v0.5.72.enumeration.md`
- `docs/reviews/v0.5.73.enumeration.md`
- `docs/reviews/v0.5.81.enumeration.md`
- `docs/reviews/v0.5.83.enumeration.md`
