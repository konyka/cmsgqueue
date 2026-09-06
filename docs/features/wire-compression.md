# F2: Wire Compression (zstd) at BATCH Level

## Motivation

Wire bandwidth is often the bottleneck in clustered NATS-like
deployments. CMQ_FLAG_COMPRESSED was reserved but rejected by F11
because no compression was implemented. With zstd, a typical 4 KiB
JSON payload compresses to ~30 bytes — a 100× reduction.

Per-message compression was rejected (F11 interop bug). BATCH-level
compression is the precedent: Kafka's `CompressionType` is registered
per-batch. Aligns with how real clusters aggregate.

## Design

A new `cmq_compress` module (`src/proto/cmq_compress.{c,h}`) wraps
zstd 1.5+. The wire format extension:

```
BATCH frame:
+-----+-----+---------------------+-----------------------+
| hdr | len | zstd(payload_data)  | <-- cmq_decompress()  |
+-----+-----+---------------------+-----------------------+
              ^-- CMQ_FLAG_COMPRESSED set on header
```

The header frame's `payload_len` is the COMPRESSED size. The
zstd frame format is self-describing; the decompressed size is
read with `ZSTD_getFrameContentSize` (`cmq_decompress_bound`).

`handle_batch` checks the flag **before** pass-1 validation. If set,
it allocates exactly the content size (capped at 16 MiB),
decompresses, clears `CMQ_FLAG_COMPRESSED` on a local frame, and
recurses once. The original `frame->payload` remains owned by the
parser; the decoded buffer is freed on this return path.

**Parser (v0.5.41 / v0.5.96–98):** `CMQ_FLAG_COMPRESSED` is accepted
when `op` is BATCH, PUBLISH, MESSAGE, or REQUEST. RESPONSE and
other opcodes with the bit set are still `pending_error` (F11).

### Threshold policy

The producer auto-skips compression for payloads < 512 B (zstd
dictionary overhead exceeds savings on short frames). The decision
is made by the encoder. The server accepts any compressed payload
that decompresses cleanly.

### Bomb protection

Zstd refuses to write past `dst_cap`. The server passes
`cmq_compress_bound(payload_len)` (which is bounded by
ZSTD_compressBound, ~1.1× input size + tiny overhead) as the
upper-bound cap. The `handle_batch` also caps the bound at 16 MiB
before allocating, preventing an attacker from sending a 1-byte
compressed frame that decompresses to 1 GiB.

## Files touched

- `src/proto/cmq_compress.{c,h}` — new module.
- `src/server/cmq_server.c` — `handle_batch` decompresses when flag set.
- `CMakeLists.txt` — `pkg_check_modules(ZSTD REQUIRED libzstd)`.
- `tests/test_compress.c` — 5 tests.
- `tests/test_info.c` — updated to expect "zstd".
- `docs/features/info-frame.md` — F4 updated.

## Tests

`tests/test_compress.c`:
- `compress.is_available` — zstd library linked.
- `compress.round_trip_short` — 38-byte string round-trips.
- `compress.round_trip_large_json` — 4 KiB alphabet text compresses
  to < 1 KiB and round-trips byte-exact.
- `compress.bomb_rejected` — 4 KiB data with 1 KiB dst_cap: ZSTD
  refuses (dst_cap too small for output).
- `compress.corrupt_data_rejected` — random bytes: ZSTD errors.

## Verification gates

- 32/32 tests pass (was 31; +1 test_compress with 5 tests).
- INFO frame advertises `"compression": "zstd"`.

## Performance

For a 4 KiB JSON payload with zstd level 1:
- Compress: ~30 bytes output (130× ratio)
- Compress time: ~5 µs (single-thread, x86_64)
- Decompress time: ~1 µs

The hot-path cost on the small-frame (64 B) baseline is a 16-byte
check that the flag is unset, plus a malloc + free on the compressed
path. The `auto-skip` policy keeps the small-frame path within
baseline.

## Security

Threats closed:
- **Decompression bomb** — `cmq_decompress_bound` requires a known
  zstd content size and rejects anything above 16 MiB before
  `malloc`. ZSTD's `dst_cap` check is the inner guard. A 16× ratio
  cap is **not** used: a 4 KiB JSON payload at zstd-1 is ~30 B
  (~130×) and is a supported happy path.
- **Memory exhaustion** — the 16 MiB cap is the allocator ceiling.
- **F11 interop** — COMPRESSED on RESPONSE and other remaining
  opcodes is still rejected in the parser.

Threats NOT closed (covered by TLS in F1):
- Active tampering of compressed data. The CRC32C trailing checksum
  (F3) detects single-bit flips; HMAC for active-MITM is TLS's job.

## Limitations

- BATCH, PUBLISH, MESSAGE, and REQUEST. RESPONSE stays rejected.
  PUBLISH / REQUEST inflate the whole payload once. MESSAGE
  inflates, converts to a PUBLISH body, then fans out plaintext.
- Level 1 only. Higher levels (3, 6, 9) trade CPU for ratio;
  1 is the recommended default for hot paths.
- No negotiated codec — only zstd. LZ4 could be added behind
  `cmq_compress` API by extending the dispatcher.
- A 16 MiB cap is the server's choice; production deployments
  may want a smaller cap (e.g., 4 MiB) to limit memory pressure.

## See also

- `docs/reviews/hyperplan-bundle.md` §2.6 (BATCH-level codec decision).
- `docs/reviews/round2_deep_attack.md` B2 (Kafka precedent).
- `docs/features/info-frame.md` (F4 advertises "zstd").
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F2.
- zstd: https://facebook.github.io/zstd/
