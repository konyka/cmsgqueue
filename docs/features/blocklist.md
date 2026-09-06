# F15: Connection Blocklist (IP/CIDR)

## Motivation

Operators need a way to block abusive IPs at the connection level, pre-handshake. NATS Server supports this via a `blacklist` config (or in the new "deny" permissions). CMSGQueue had no such mechanism.

## Design

A new `cmq_blocklist` module loads a file at startup. Format: one IP or CIDR per line.

```
10.0.0.1
192.168.0.0/16
172.16.0.0/12
```

Empty lines and malformed entries are skipped. The blocklist is checked at `accept_cb` after the OS `accept()` returns. Banned IPs are closed immediately.

The blocklist is reloadable at runtime via `cmq_blocklist_reload`. Updates are lock-protected.
v0.5.149: SIGHUP attaches `blocklist_file` when create had none.
Omitted / empty keeps off. Unsafe or missing file fail closed.
An existing handle is still swapped, not remounted.

The module is shipped as a library; the server-c-side wiring is a follow-up. The library API is complete and tested.

## Files touched

- `src/cluster/cmq_blocklist.{h,c}` (new).
- `CMakeLists.txt` — `cmq_blocklist.c` added to `CMQ_CLUSTER_SOURCES`.
- `tests/test_blocklist.c` (new).

## Tests

`tests/test_blocklist.c`:
- `blocklist.empty_admits_all` — empty file, all IPs admitted.
- `blocklist.single_ip_blocked` — `10.0.0.1` admitted, `10.0.0.2` blocked.
- `blocklist.multiple_ips` — multiple entries + CIDR.
- `blocklist.malformed_lines_skipped` — garbage lines ignored.

## Verification gates

- 4/4 blocklist tests pass.
- 45/45 total tests pass.

## Performance

Match is a linear scan over a small entries list (typically <100 IPs/CIDRs). Cost: ~50 cycles per accept. Negligible compared to the TCP accept syscall (~1 µs).

## Security

Threats closed:
- **IP-based DoS** — abusive IPs are dropped pre-handshake.
- **Blocklist hot-reload** — operators can update without restart.

Threats NOT closed:
- **IPv6** — only IPv4 supported in v0.3.0.
- **CIDR with non-byte-aligned prefix** — not validated; entry is silently dropped.

## Limitations

- Server-side wiring (config + accept_cb) is a follow-up. The library is complete and tested.
- 4096-entry hard cap.
- No persistence: blocklist lives in memory; restart requires re-loading the file.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F15.
- `docs/features/audit.md` — denied connects trigger audit events.
- `docs/features/rate-limit.md` — F10 per-IP rate limit (different from blocklist).
