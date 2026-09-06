# HPACK codec (v0.5.66–71, D2 phases 1 and 3)

Library encode/decode for HPACK integers, literal strings,
Huffman, and static indexed headers (RFC 7541).

## Limits

- Integers reject above 2^20.
- Literal strings: H=0 raw encode/decode; H=1 Huffman
  decode via `cmq_hpack_huff_decode`.
- Huffman leftover padding must be EOS ones and ≤7 bits.
- Static table only (indexes 1–61). Dynamic table size 0.
- 4 KiB dynamic table and the h2 listener stay deferred.

The server still does not call `cmq_tls_set_alpn` and does
not advertise `h2`.

## Tests

`tests/test_hpack.c`, `tests/test_huff.c`

## See also

- `docs/reviews/v0.5.66.enumeration.md`
- `docs/reviews/v0.5.71.enumeration.md`
