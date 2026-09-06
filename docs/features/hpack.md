# HPACK static codec (v0.5.66, D2 phase 1)

Library encode/decode for HPACK integers, literal strings,
and static indexed headers (RFC 7541).

## Limits

- Integers reject above 2^20.
- Literal strings are raw (Huffman bit rejected).
- Static table only (indexes 1–61). Dynamic table size 0.
- 4 KiB dynamic table and the h2 state machine stay deferred.

The server still does not call `cmq_tls_set_alpn` and does
not advertise `h2`.

## Tests

`tests/test_hpack.c`

## See also

- `docs/reviews/v0.5.66.enumeration.md`
