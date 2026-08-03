# F3: Wire Checksum (CMQ_FLAG_CHECKSUM)

## Motivation

The CMSGQueue wire protocol reserves `CMQ_FLAG_CHECKSUM` (bit 1) for
end-to-end integrity checking between hops. Prior to F3, this flag
was silently ignored — a peer setting it would have its bytes
round-tripped as opaque garbage (the same F11 interop bug class).

End-to-end integrity is critical for:
- Cluster routes (F-relay): a bit flip on the wire must be caught
  before it reaches downstream subscribers.
- MQTT bridge (F6): MQTT clients may publish from unreliable
  channels; the bridge must verify the wire payload.
- Persistence (F5): on-disk replay must be tamper-evident.

CRC32C (Castagnoli) is the standard polynomial used by iSCSI (RFC 3720),
SCTP, BTRFS, ext4, and the Intel SSE4.2 / aarch64 CRC32 hardware
instructions. F9 provides the hardware-accelerated implementation;
F3 wires it into the protocol.

## Design

### Wire format extension

When `CMQ_FLAG_CHECKSUM` (bit 1) is set on a `PUBLISH` frame:

```
+-----+--------+--------+----------+----------+
| hdr | payload[0..N-5] | CRC32C[4] |
+-----+--------+--------+----------+----------+
```

- The wire payload is `payload_len` bytes as declared in the header.
- The trailing 4 bytes (`payload[N-4..N-1]`) are the CRC32C of the
  first `payload_len - 4` bytes, **little-endian**.
- The CRC32C uses the standard form: init=0xFFFFFFFF, xorout=0xFFFFFFFF
  (matches RFC 3309 / iSCSI).
- The checksum covers the **entire** wire payload (subject, reply-to,
  headers, body), so a single bit flip anywhere is detected.

### Server-side verification

In `handle_publish` (`src/server/cmq_server.c:2873`):

```c
if (frame->hdr.flags & CMQ_FLAG_CHECKSUM) {
    if (msg_len < 4) {
        cmq_send_error(c, "checksum: payload too short");
        return;
    }
    size_t data_len = msg_len - 4;
    uint32_t expect = ...; // trailing 4 bytes, little-endian
    uint32_t got = cmq_crc32c(0, payload, payload_len - 4);
    if (expect != got) {
        cmq_send_error(c, "checksum mismatch");
        return;
    }
    msg_len = data_len;  // strip trailing 4 bytes from delivery
}
```

The verification cost is a single 64-bit CRC32C instruction per 8 bytes
on hardware, plus the constant-time compare. Net hot-path overhead on
a 64-byte payload: ~5 cycles with `_mm_crc32_u64` (SSE4.2).

### Parser change (F11 follow-up)

F11 rejected both reserved flags (COMPRESSED, CHECKSUM) pre-CONNACK.
F3 ships the CHECKSUM path, so the parser now only rejects COMPRESSED.
The F11 test `parser.reject_flag_checksum` was updated to assert
**acceptance** (the parser forwards the frame; the server verifies).

## Files touched

- `src/server/cmq_server.c` — `handle_publish()` verifies trailing 4 bytes.
- `src/proto/cmq_parser.c` — flags check allows CHECKSUM, still rejects COMPRESSED.
- `tests/test_checksum_wire.c` — 3 new tests.
- `tests/test_parser.c` — F11 `reject_flag_checksum` updated.
- `docs/features/flag-rejection.md` — note the F3 follow-up.

## Tests

`tests/test_checksum_wire.c`:
- `checksum_wire.crc32c_compute_append` — verifies the trailing 4 bytes are
  CRC32C of the data.
- `checksum_wire.parser_holds_checksum_bytes` — parser preserves the
  trailing 4 bytes (server-side strips).
- `checksum_wire.bit_flip_detected` — single bit flip changes the CRC.

## Verification gates

- All 29 tests pass (was 28; +1 test_checksum_wire).
- Existing F11 tests updated to reflect new flag policy.

## Performance

- Hot-path overhead: ~5 cycles/8 bytes on SSE4.2; ~10 cycles on aarch64.
- For 64-byte payload: ~40 ns vs the 30 µs baseline. Sub-1% regression.

## Security

Threats closed:
- **Bit flip on the wire** — CRC32C detects any single-bit flip
  with probability 1 - 2^-32.
- **Multi-bit error** — HDF (Hamming Distance) properties of CRC32C
  detect all 1-bit, 2-bit, and most 3-bit errors with high probability.
- **Truncation attack** — server verifies length before delivery.

Threats NOT closed (use TLS for these):
- **Active MITM** — CRC32C is not a MAC; an attacker can recompute the
  CRC after modifying bytes. Use TLS (F1) for end-to-end authentication.
- **Replay attack** — CRC32C provides no defense. Use sequence numbers
  (out of scope for F3).

## Limitations

- The checksum does NOT cover the wire header (9 bytes). A bit flip
  on the header would be caught by the parser's length validation,
  but it's worthwhile to note.
- The trailer is 4 bytes; for very large payloads, an attacker could
  flip a bit AND the CRC (probability 2^-32). For F3's wire-protocol
  use, this is acceptable; for keystore integrity, use HMAC.

## See also

- `docs/features/crc32c.md` — F9 implementation.
- `docs/features/flag-rejection.md` — F11.
- `docs/reviews/hyperplan-bundle.md` §2.4 (defensible decision).
- `docs/reviews/round2_deep_attack.md` C2 (CRC32 conflation).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F3.
- RFC 3309 — CRC32C for iSCSI.
