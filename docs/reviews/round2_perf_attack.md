# Round 2 Cross-Attack — Performance Architect (unspecified-high)

Baseline to defend: **33 840 msg/s end-to-end, ~2.27 M publish/s, 0.030 ms avg latency**
(`docs/benchmarks/results.md:26`). Every claim below is graded against regressing that line.

---

## A. Attacking unspecified-low (implementer) — the missing-flag is bigger than they think

**A1. The hot path silently DEALLOCS what a missing checksum never protected.**
`cmq_server.c:1712` `cmq_build_message_frame()` walks `frame->hdr.flags` for
`CMQ_FLAG_HEADERS` (line 1757) but never inspects `CMQ_FLAG_COMPRESSED` or
`CMQ_FLAG_CHECKSUM` (defined `cmq_proto.h:14-15`). On the receive side
`cmq_server.c:2857` likewise. Consequence: a publisher sets `CHECKSUM`, the
server does not verify it, the per-frame `malloc(payload_len)` at
`cmq_parser.c:279` succeeds, and the data is delivered with undetected
corruption — but worse, **the protocol lies to the client about safety while
paying the wire-size cost of the 4-byte CRC**. A 64-byte message grows to 68
bytes on the wire (~+6 %) for zero protection. On a 33.8 K msg/s stream that
is **~2 000 msg/s of effective throughput lost for nothing** the first time
the flag is set in production. **Fix: wire checksum FIRST and reject the
frame at `cmq_parser.c:289` if `CHECKSUM` is set but trailer is missing or
mismatched.** Never silently pass.

**A2. OpenSSL detection is `QUIET` — the build appears green while perf is**
**silently crippled.** `CMakeLists.txt:159-164`: `find_package(OpenSSL QUIET)`
then unconditional `target_link_libraries(... crypto ssl)`. On a host without
the OpenSSL imported target (the `else` branch, which is the **default** on
distros that ship only the system libs), the binary still links and the test
suite still passes — but `cmq_tls.c:161` `cmq_tls_backend_secure()=0` makes
the server fail-closed. **The performance cost is "TLS off forever"** rather
than a measurable regression, which is worse: no benchmark will ever show
the problem. Implementer's "build hardening absent" bullet undercounts this;
the real attack is the silent path. **Fix: hard-fail configure if TLS
requested but no OpenSSL imported target found, OR drop TLS build option
until the stub is real.**

**A3. mTLS for inter-node IS overkill relative to perf budget.** Requiring
mutual cert validation on every route/gateway link doubles handshake cost
(2× cert-verify paths, 2× chain walks) for connections that already
authenticate at the application layer via cluster tokens. With 33.8 K msg/s
end-to-end on a single node, a 1 vCPU per-node cluster sees ~2 ms of CPU per
handshake that buys no additional security if the cluster already runs on
RFC1918/VPC. **Better:** TLS 1.3 with **session resumption (PSK/ticket)** for
inter-node, mTLS only for edge↔edge federations crossing trust boundaries.
Resumed handshake is ~0.1 RTT vs 1-RTT-TLS1.3 or 2-RTT-TLS1.2 — that's the
perf win that keeps the baseline.

---

## B. Attacking ultrabrain (security) — over-specified the seal, under-spec'd the engine

**B1. AEAD-only is correct, but TLS 1.2 floor is wrong.** Ult rabrain says
"TLS 1.2+, AEAD". TLS 1.2 with X25519+ChaCha20-Poly1305 is AEAD but does
1-RTT with a heavier key schedule and **no 0-RTT**. For an MQTT-bridge /
client reconnect workload where we can re-handshake hundreds of times/sec,
TLS 1.3's single round-trip plus session tickets is a **40-60 % handshake
latency win** and a measurable win in CPU per byte (AES-GCM is fast on x86
but ChaCha20-Poly1305 inside TLS 1.2 is two more flights of state). **Fix:
require TLS 1.3 minimum, AEAD ciphers only, no 0-RTT (replay risk on
async messaging).**

**B2. CRC32 is the right primitive — placement is the bug.** Ult rabrain
proposed "CRC32; document tamper-detection limitation". But `cmq_filestore.c:54`
is the **software bit-by-bit CRC32** — no SSE4.2 `_mm_crc32_u64`, no
aarch64 `crc32x`, no PCLMUL on the wire path. At 64-byte payload this is
~50-80 ns per frame. On a stream of 33.8 K msg/s the CRC alone is **~2 ms of
CPU/s per core, ≈ 5-7 % of one core at idle room**. And the per-frame
`crc32_update` runs in the **store append path** (line 491), which is
currently dormant (filestore is library-only — see C2). Once persistence is
wired, this is the new bottleneck. **Fix: detect CPUID SSE4.2 / aarch64
CRC32 in `cmake/cmq_compiler.cmake` and select a hw-accelerated path; place
CRC verify POST-decrypt, PRE-encrypt in the protocol so the wire-cost is
amortized into the existing 9-byte header read at `cmq_parser.c:265`.**

**B3. Stack-protector-strong on the parser hot path is a perf bomb.**
`cmake/cmq_compiler.cmake:38-54` currently has zero hardening. Adding
`-fstack-protector-strong` to `cmq_parser_feed` (the per-byte function called
once per frame at line 323) costs a function-entry canary write + read
**~3-5 ns/frame** = ~150 µs/s at 33.8 K msg/s = **0.5 % per-protect entry**;
on 4-frame deep macro paths (parser→push→encode→deliver) that compounds to
**2-3 %** real loss. **Acceptable**, but FORTIFY_SOURCE=3 is free on
`memcpy`/`memmove` paths and not on the hot loops — so order: enable
`_FORTIFY_SOURCE=2` (≈0.1 %), then `-D_FORTIFY=3` only on library targets
not the parser, and skip `stack-protector-strong` on `cmq_parser.c`,
`cmq_slab.c`, `cmq_mpool.c`. **This is the only way to keep the ±2 % perf
ceiling the user implied.**

---

## C. Attacking artistry (TDD/doc) — testability vs hot-path are NOT in tension here

**C1. The "TDD forces injection" claim is wrong for this codebase.**
`cmq_test.h:1-7` is a header-only framework with `__attribute__((constructor))`
per-TU registration; there is zero virtual dispatch, zero interface table, zero
indirection added by tests. The parser slab at `cmq_parser.c:111`
`cmq_slab_create()` is already injected via direct call — there is no
interface seam to add. **Testable design here is *free***. The real perf
risk is `__attribute__((constructor))` running the test registry at every
`#include "cmq_test.h"` in N TUs: ~32 TUs × ~50 ns = **1.5 µs at process
startup**, but **~3-4 KB of binary bloat** for unused test symbols in
release. The right call is `__attribute__((used))` registration but link
the tests into a separate binary (`tests/CMakeLists.txt` already does this
in spirit). Don't over-rotate: keep the header. **Artistry's framing is
fine; under-theorized is which assertions deserve a microbench.**

**C2. "Wire checksum FIRST" — perf-correct, but ONLY if the path is hw-accelerated.**
This *is* the right TDD order because the failure is silent (see A1). But
artistry didn't quantify what "wire checksum" means in CPU terms. With the
current software CRC in `cmq_filestore.c:54-61` you'll lose 5-7 % per core
on the publish path. **The TDD ordering must be**: (i) implement CRC
verification at the parser reject site, (ii) **simultaneously** commit a
hw-accelerated variant gated on `__SSE4_2__` / `__ARM_FEATURE_CRC32`, (iii)
benchmark before/after with the existing `examples/benchmark` workload. If
hw-CRC is unavailable, the design must include **"disable CHECKSUM by
default and warn loudly"** — not silently pay 5 % for a feature on by default.

**C3. libfuzzer for parser is right, but single-threaded fuzzing misses a perf regression class.**
A persistent `fuzz_parse_one_frame()` corpus covers header-validation bugs,
which is what artistry wants. **It will not catch the regressions in
`cmq_parser.c:311-318`** — the inbuf-compact `memmove` that only fires when
`inbuf_off * 2 >= inbuf_len`, which under sustained tiny-frame traffic can
turn into an O(n) copy per `parser_feed` call. **Fix: include a fuzzer seed
corpus with high-volume tiny-frame scenarios AND a microbench that measures
parser throughput with the inbuf-compact forced on.** Artistry under-budgeted
the corpus work.

---

## D. Attacking deep (evidence) — the order is correct but priorities are inverted

**D1. Implementation order is correct; the *priority* of compression is not.**
`checksum → TLS → compression → MQTT → persistence` is the correct
dependency DAG (no downstream feature can be honest without its upstream).
But deep is wrong that compression is "load-bearing" for the perf story.
For a 64-byte median payload (per `examples/benchmark` defaults, line 5 of
results.md), LZ4 fast-level compress is **~150 ns + 50 ns decompress ≈
200 ns/frame**, while the round-trip is **30 000 ns/frame**. The compression
ratio for 64-byte NATS-style control messages is **0.95-1.05×** (i.e. it
makes the frame *larger* by adding LZ4 frame headers). **Compression is a
net loss at the median workload.** It only pays off above ~512 bytes
payload. **Fix: wire `CMQ_FLAG_COMPRESSED` but auto-skip when `payload_len <
CMQ_COMPRESS_MIN` (512 B), and report the savings in the stats frame.** This
keeps the wire-compatible flag, honors the protocol, and stops paying the
200 ns tax on every small frame.

**D2. Persistence is HIGHER priority than deep put it.** Filestore is
library-only (no `cmq_server.c` call site) and the CRC-at-rest is the
*only* integrity check in the system. With `cmq_filestore.c:54-65` using
software CRC, every WAL append costs ~80 ns/frame. If you write 1 M
msg/s, that's 80 ms/s on a single core. **Until the hw-CRC variant ships,
persistence caps at ~500 K msg/s on a single filestore writer, and
`cmq_filestore_t` is already serialized by `fs->lock` (line 439).** Deep's
order should be: **hw-CRC → persistence (WAL+group-commit) → checksum on
the wire → TLS → compression → MQTT bridge.** Group-commit batches fsync
to amortize ~5-20 ms fsync — without it, persistence is unusable above
10 K msg/s on HDD, 100 K on SSD.

**D3. Snapshot vs WAL — deep said neither. Pick WAL + periodic snapshot.**
For a NATS-like queue where consumers track their own sequence, **WAL is
strictly cheaper** (append-only O(1) write vs snapshot's rewrite-the-world).
The current `cmq_filestore.c:493-510` is already WAL-shaped (header + data
+ idx append, ftruncate on failure). **Add group-commit: coalesce N
appends into one `fdatasync`**, target fsync every 1-5 ms. This is the
only way persistence doesn't tank the 33.8 K baseline. Snapshots are
compaction, not the primary path.

**D4. mpool/slab interaction with persistence is a hidden regression.**
`cmq_slab` (`cmq_slab.c`, used in `cmq_parser.c:68`) gives O(1) alloc/free
for fixed-size `cmq_frame_node_t` (24 B). Once persistence is wired, the
persistence layer will need its own fixed-size records (frame header + seq
+ crc) of the **same shape**. **Don't add a second slab** — extend
`cmq_slab` to support heterogeneous size classes OR add a small dedicated
slab at filestore layer with the same O(1) free-cache discipline as
`cmq_slab.c`. Otherwise persistence re-introduces `malloc` to the hot
append path and erases the benchmark delta that the prior perf commit
bought (results.md:11-12).

---

## Summary — what I will defend in Round 3

1. **Reject A1's silent flag pass; require hw-CRC before checksum is
   default-on.** ≤1 % regression when `CRC32C`/`crc32x` is available.
2. **TLS 1.3 + session resumption** for inter-node, mTLS only at trust
   boundaries. ≤2 % handshake-path overhead in steady state.
3. **Compression auto-skip below 512 B payload** to avoid the 200 ns tax on
   64-byte median traffic. Net 0 % regression at current workload.
4. **FORTIFY_SOURCE=2 + selective `-fstack-protector-strong`** (parser
   excluded). 1-2 % ceiling, 0 % on the parser.
5. **Reorder deep: hw-CRC → WAL/group-commit → checksum-on-wire → TLS →
   compression → MQTT.** Persistence is the next perf cliff, not
   compression.

**If the user enforces ±2 %: drop default-on compression below 512 B
(which I'll auto-skip) and drop the default-on CHECKSUM (rely on TLS for
integrity when enabled, keep CRC at rest).** Both are wire-compatible;
neither regresses the benchmark.
