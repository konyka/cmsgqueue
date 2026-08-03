# F9: Hardware-Accelerated CRC32C with Software Fallback

## Motivation

The CMSGQueue wire protocol reserves `CMQ_FLAG_CHECKSUM` (F3) for
end-to-end integrity checking between hops (cluster routes, MQTT
bridge, file store). The existing in-tree CRC32 lives in
`src/store/cmq_filestore.c:54` and is a **software bit-by-bit** loop
using the IEEE 802.3 polynomial (Ethernet). This differs from CRC32C
(Castagnoli) used by iSCSI, RFC 3309, SCTP, and virtually all modern
checksum standards.

Software bit-by-bit is ~16× slower than hardware-accelerated on x86_64
(SSE4.2 CRC32) and aarch64 (CRC32CX). For the wire checksum (F3) to
be viable without regressing the 33 K msg/s baseline, the CRC must
use hardware acceleration where available.

## Design

A new `cmq_crc32c` module in `src/core/`:

- **Public API** (`cmq_crc32c.h`):
  - `cmq_crc32c(crc, data, len)` — standard CRC32C with
    init=0xFFFFFFFF, xorout=0xFFFFFFFF. Pass 0 to start, chain by
    passing the previous return value.
  - `cmq_crc32c_raw(crc, data, len)` — raw CRC32C with init=crc,
    xorout=0. For streaming where the caller manages init/xorout.
  - `cmq_crc32c_is_hw()` — reports whether the hardware path is active.

- **Implementation** (`cmq_crc32c.c`):
  - **Hardware path**: compile-time selection.
    - x86_64 with SSE4.2: `_mm_crc32_u64` (8 bytes at a time).
    - aarch64 with `__ARM_FEATURE_CRC32`: `__crc32cd` (8 bytes at a time).
    - else: software bit-by-bit fallback.
  - **Tail processing**: bytes that don't fit a 64-bit chunk are processed
    one at a time via byte variants (`_mm_crc32_u8`, `__crc32cb`, or
    software bit-by-bit).
  - **Standard form**: the public `cmq_crc32c` inverts the running CRC
    on entry and exit, since hardware instructions use init=0, xorout=0
    but the standard (RFC 3309) form uses init=0xFFFFFFFF, xorout=0xFFFFFFFF.

## CRC32C vs IEEE 802.3 (CRC32)

The codebase has two CRCs:

| Module | Polynomial | Used by |
|---|---|---|
| `cmq_filestore.c:crc32_*` | IEEE 802.3 (0xEDB88320 reflected) | On-disk record format (legacy) |
| `cmq_crc32c` (this PR) | Castagnoli (0x82F63B78 reflected) | F3 wire checksum, F5 persistence WAL |

These are intentionally different. Migrating on-disk format would
require a versioned reader. The new module is additive.

## Files touched

- `src/core/cmq_crc32c.h` — public API.
- `src/core/cmq_crc32c.c` — implementation.
- `CMakeLists.txt` — added to `CMQ_CORE_SOURCES`.
- `cmake/cmq_compiler.cmake` — adds `-msse4.2` on x86_64 (was -msse2 only).
- `tests/test_crc32c.c` — 7 tests with RFC 3309 reference vectors.

## Reference vectors

```
crc32c("")              = 0x00000000
crc32c("123456789")     = 0xE3069283  (RFC 3309)
crc32c("The quick brown fox jumps over the lazy dog") = 0x22620404
```

## Tests

`tests/test_crc32c.c`:
- `crc32c.empty_input` — passes 0 through.
- `crc32c.rfc3309_vector_123456789` — RFC 3309 vector.
- `crc32c.fox_vector` — RFC 3309 / well-known fox vector.
- `crc32c.streaming_consistency` — one-shot vs 1-byte chunks equal.
- `crc32c.hw_sw_equivalence` — deterministic across 1000 iterations.
- `crc32c.bit_flip_detected` — single bit flip changes the CRC.
- `crc32c.large_payload` — 64 KB payload exercises hw path.

## Performance

Expected speedups vs software bit-by-bit (filestore model):
- x86_64 SSE4.2: ~16× on 64 KB payload.
- aarch64 CRC32: ~12× on 64 KB payload.
- Software fallback: identical to existing.

The wire checksum (F3) at 64 B median payload would add:
- hw path: ~5 ns/frame (1 cycle/byte on CRC32).
- sw path: ~80 ns/frame.
- This is the **gating reason** for F9 to ship before F3.

## Verification gates

- All 7 CRC32C tests pass.
- All 28 tests pass (was 26, +1 hardening, +1 crc32c).
- `cmq_crc32c_is_hw()` returns 1 on x86_64 with SSE4.2 (verified).

## See also

- `docs/reviews/hyperplan-bundle.md` §2.4 (defensible decision).
- `docs/reviews/round2_deep_attack.md` C2 (CRC32 conflation).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F9.
- RFC 3309 — CRC32C for iSCSI.
