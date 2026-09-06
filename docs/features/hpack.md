# HPACK codec (v0.5.66–72, D2 phases 1, 3–4)

Library encode/decode for HPACK integers, literal strings,
Huffman, static indexed headers, and a 4 KiB dynamic table
(RFC 7541).

## Limits

- Integers reject above 2^20.
- Literal strings: H=0 raw encode/decode; H=1 Huffman
  decode via `cmq_hpack_huff_decode`.
- Huffman leftover padding must be EOS ones and ≤7 bits.
- Static table indexes 1–61. Dynamic indexes start at 62.
- Dynamic table: 4 KiB RFC size, 128 slots, no heap.
  Incremental literals insert; indexed static fields do not.
  Size updates and `set_max` reject above 4096.
- The h2 listener stays deferred.

The server still does not call `cmq_tls_set_alpn` and does
not advertise `h2`.

## Tests

`tests/test_hpack.c`, `tests/test_huff.c`, `tests/test_hdyn.c`

## See also

- `docs/reviews/v0.5.66.enumeration.md`
- `docs/reviews/v0.5.71.enumeration.md`
- `docs/reviews/v0.5.72.enumeration.md`
