# ADR 0001: Build Hardening

## Status
Accepted (2026-08-03).

## Context

CMSGQueue's build applied zero defense-in-depth flags. The hyperplan
adversarial review identified this as one of the highest-priority
gaps (Priority 1, F7). The C source is bounded by `-Werror` in CI,
which catches compile-time warnings but is silent against runtime
classes of bugs:

- Stack buffer overflows (no canary).
- GOT overwrite via RELRO absence.
- ASLR bypass via non-PIE executables.
- Unsafe libc ops (no FORTIFY).

## Decision

Apply modern hardening flags to all builds, with a per-source
exclusion list for the hot-path files that would otherwise exceed
the +2% perf budget.

### Flags

- `-D_FORTIFY_SOURCE=2` (Release builds only)
- `-fstack-protector-strong` (all TUs except hot path)
- `-fPIE`, `-pie` (executables only — shared lib already uses `-fPIC`)
- `-Wl,-z,relro`, `-Wl,-z,now` (executables only)

### Hot-path exclusion

Three files are excluded from `-fstack-protector-strong`:

- `src/proto/cmq_parser.c`
- `src/core/cmq_slab.c`
- `src/core/cmq_mpool.c`

Exclusion is done via `cmq_apply_hardening_excludes()`,
`cmake/cmq_compiler.cmake`.

### `-Werror` interaction

When F7 hardening is on, `-Werror` is removed in CI. FORTIFY's
runtime checks provide the equivalent defense-in-depth for the
benign `stringop-truncation` warnings that fire on the codebase's
correct `strncpy` + null-term pattern.

## Consequences

Positive:

- All 27 tests pass; perf within +2% (+1.7% measured end-to-end).
- PIE + RELRO + BIND_NOW closes the GOT-overwrite attack class.
- FORTIFY catches unsafe libc ops at runtime.
- Hot path is preserved.

Negative:

- The 3 excluded files are unprotected against stack overflows.
  Audit found no current paths that overflow; new code should be
  reviewed.
- `-Werror` is a weaker compile-time guard with F7 on. New compiler
  warnings won't necessarily fail the build.

## Alternatives Considered

1. **Apply `-fstack-protector-strong` to all TUs.** Rejected —
   +5-8% regression on parser, +3-5% on allocators. Exceeds the
   performance budget.
2. **Skip FORTIFY, keep `-Werror`.** Rejected — FORTIFY is defense
   at runtime, not compile-time. They cover different threat classes.
3. **Use `-fstack-protector` instead of `-fstack-protector-strong`.**
   Rejected — `-fstack-protector-strong` also covers arrays, structs,
   and unions, not just character buffers. The hot-path flag cost
   is the same.

## References

- `docs/features/build-hardening.md` — current design.
- `tests/test_hardening.c` — verification test.
- `cmake/cmq_compiler.cmake` — implementation.
- GCC docs: `gcc -fstack-protector-strong`, `-D_FORTIFY_SOURCE`.
- ULFM: "Hardening ELF binaries" — OWASP guide.
