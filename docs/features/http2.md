# HTTP/2 (v0.5.66–72, D2 phases 1–4)

HPACK static + Huffman + 4 KiB dynamic table plus an
HTTP/2 connection state machine.

## Frame layer (v0.5.69)

- Preface: `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` (24 bytes).
- 9-byte frame header. Payload cap 16 KiB.
- First frame after preface must be SETTINGS (not ACK).
- Max 32 concurrent streams. Client stream ids must be odd.
- A 33rd stream or a protocol error moves the conn to GOAWAY.

Huffman is v0.5.71. The 4 KiB dynamic table is v0.5.72. A
listener stays deferred. The server still does not call
`cmq_tls_set_alpn` and does not advertise `h2`.

## Tests

`tests/test_hpack.c`, `tests/test_huff.c`, `tests/test_hdyn.c`,
`tests/test_h2.c`

## See also

- `docs/reviews/v0.5.66.enumeration.md`
- `docs/reviews/v0.5.69.enumeration.md`
- `docs/reviews/v0.5.71.enumeration.md`
- `docs/reviews/v0.5.72.enumeration.md`
