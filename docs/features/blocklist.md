# F15: Connection Blocklist (IP/CIDR)

## Motivation

Operators need a way to block abusive IPs at the connection level, pre-handshake. NATS Server supports this via a `blacklist` config (or in the new "deny" permissions). CMSGQueue had no such mechanism.

## Design

A new `cmq_blocklist` module loads a file at startup. Format: one IP or CIDR per line.

```
10.0.0.1
192.168.0.0/16
172.16.0.0/12
2001:db8::1
2001:db8:1234::/64
::/0
```

Empty lines and malformed entries are skipped. IPv4 and IPv6 entries support
exact addresses and every CIDR prefix from `/0` through `/32` or `/128`.
The library can match IPv4 and IPv6 `sockaddr` values through
`cmq_blocklist_check_sockaddr`; the existing `cmq_blocklist_check` IPv4 API is
unchanged for ABI compatibility.

The primary and configured secondary server listeners support IPv4 and IPv6
addresses and close blocked peers immediately in both accept paths, before
handshake or admission. TLS-listener expansion remains deferred.

The blocklist is reloadable at runtime via `cmq_blocklist_reload`. Updates are lock-protected.
v0.5.149: SIGHUP attaches `blocklist_file` when create had none.
Omitted / empty keeps off. Unsafe or missing file fail closed.
v0.5.154: an existing handle is swapped through
`cmq_blocklist_reload_swap` (fail-closed + live path).
In-flight readers keep the old list until the rch swap.

The module is shipped as a library. Library IPv6 support is complete and
tested; server integration covers the primary and configured secondary
listeners.

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
- `blocklist.ipv6_exact_and_prefixes` — IPv6 exact, `/64`, and `/0` matches.
- `blocklist.ipv6_non_match_and_mixed_families` — IPv6 non-match plus mixed input.
- `blocklist.ipv6_malformed_prefixes_skipped` — invalid IPv6 prefixes ignored.

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
- **TLS-listener expansion** — additional TLS listener integration remains deferred.

## Limitations

- TLS-listener expansion is a follow-up; the blocklist API supports both IPv4
  and IPv6.
- 4096-entry hard cap.
- No persistence: blocklist lives in memory; restart requires re-loading the file.

## See also

- `docs/reviews/hyperplan-v030-plan.md` F15.
- `docs/features/audit.md` — denied connects trigger audit events.
- `docs/features/rate-limit.md` — F10 per-IP rate limit (different from blocklist).
