# F11: Wire Flag Rejection (Interop Bug Fix)

## Motivation

The CMSGQueue wire protocol reserves two flag bits:

- `CMQ_FLAG_COMPRESSED = 0x01` (defined in `src/proto/cmq_proto.h:14`)
- `CMQ_FLAG_CHECKSUM   = 0x02` (defined in `src/proto/cmq_proto.h:15`)

Both are **reserved but not yet implemented on the wire**. Prior to this fix, a
peer setting either bit on a `PUBLISH` frame was successfully parsed — the
parser recorded the flag bits in `frame.hdr.flags`, then re-emitted them on
outbound `MESSAGE` frames (`src/server/cmq_server.c:2915/2956/2983`). The
result: a compressed PUBLISH **round-tripped as opaque plaintext bytes** to
every subscriber in the fanout tree.

This is a **silent interop bug** — no error, no logging, just garbage fanned
out to subscribers. The hyperplan adversarial review identified it as the
highest-priority weakness preceding any new feature work.

## Design

**Reject unknown flag bits pre-CONNACK.** The parser inspects the flags byte
in `parser_parse_inbuf` (`src/proto/cmq_parser.c:252`) and marks
`pending_error = 1` when bits 0 or 1 are set. The server's accept loop
already drains the queue and tears down the connection on `pending_error`,
so the offending peer is disconnected cleanly.

The fix is **fail-closed**: a feature that hasn't been implemented on the
wire must NOT silently round-trip garbage. The intent is for future
promotion: each bit moves to a dedicated branch in the parser that knows
how to handle the framing.

**Compatibility:**
- `CMQ_FLAG_HEADERS (0x04)` — already implemented, still passes.
- `CMQ_FLAG_BATCH (0x08)` — already implemented, still passes.
- `CMQ_FLAG_ROUTE (0x10)` — used on CONNECT, not in the parser path.
- `CMQ_FLAG_COMPRESSED (0x01)` — now rejected.
- `CMQ_FLAG_CHECKSUM (0x02)` — now rejected.
- Any combination of reserved bits — now rejected.

Forward path: when `CMQ_FLAG_COMPRESSED` is implemented (F2), the parser
gains a branch that knows how to decompress the payload. Same for `F3`
(checksum). Each promotion is a TightlyScoped PR.

## API

No public API change. The parser's behavior is observable only via:
- `cmq_parser_pending_error(p)` — returns 1 on bad flag.
- `cmq_parser_feed(p, ...)` — returns 1 with one queued frame (drain semantics)
  or -1 on fatal.

## Tests

`tests/test_parser.c`:
- `parser.reject_flag_compressed` — flag 0x01 → pending_error.
- `parser.reject_flag_checksum` — flag 0x02 → pending_error.
- `parser.reject_flag_combined_reserved` — flag 0x03 → pending_error.
- `parser.accept_flag_headers` — flag 0x04 still passes (sanity).
- `parser.accept_flag_batch` — flag 0x08 still passes (sanity).

## Verification Gates

- New tests fail before implementation, pass after.
- Existing 26 tests pass (no regression).
- Manual: client sets flag 0x01, server disconnects with `pending_error`.

## Performance

The flag check is one bitwise-AND on a hot path. Cost: ~1 cycle/frame on
x86_64. Indistinguishable from the baseline of 33 K msg/s.

## Security

Threat model: a peer setting `CMQ_FLAG_COMPRESSED` could:
- Trigger a future decompression-bomb attack if a buggy implementation
  passes raw bytes to `ZSTD_decompress` (CWE-409).
- Side-channel attack via compression ratio (CWE-200 / CRIME-class).

By rejecting the bit pre-CONNACK, we close this entire class of attack
until the feature is implemented with proper bounds checks.

## See Also

- `docs/reviews/hyperplan-bundle.md` §1, F11 (catalogued finding).
- `docs/reviews/round2_deep_attack.md` A1 (formal verification).
- `src/proto/cmq_parser.c:252` (implementation).
- `tests/test_parser.c` (tests).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F11.
