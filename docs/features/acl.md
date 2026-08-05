# F16: Per-Account Subject ACL

## Motivation

Some deployments need fine-grained subject access control per account. NATS Server has `accounts { users { permissions { publish = [...], subscribe = [...] } } }`. CMSGQueue has the `cmq_account` library but no per-subject rules.

## Design

A new `cmq_acl` module supports allow-list and deny-list patterns with NATS-style wildcards:

| Pattern | Matches |
|---|---|
| `foo.bar` | exact |
| `foo.*` | one token after `foo.` |
| `foo.>` | any depth after `foo.` |

Semantics:
- Empty lists: admit everything.
- Deny-list only: admit unless denied.
- Allow-list only: admit only if matched.
- Both: deny-list wins (defense in depth).

Per-account ACL is layered: each `cmq_account` has a `cmq_acl_t *acl`. The check is in `handle_publish` before `cmq_sublist_match`. On deny, `cmq_send_error("permission denied")`.

## Files touched

- `src/enterprise/cmq_acl.{h,c}` (new).
- `CMakeLists.txt` — `cmq_acl.c` added to `CMQ_ENTERPRISE_SOURCES`.
- `tests/test_acl.c` (new).

## Tests

`tests/test_acl.c`:
- `acl.empty_admits_all` — no rules.
- `acl.exact_allow` — exact match.
- `acl.single_token_wildcard` — `foo.*` matches `foo.bar` but not `foo.bar.baz`.
- `acl.full_wildcard` — `foo.>` matches `foo.bar.baz`.
- `acl.deny_list_wins` — `foo.admin` denied even with `foo.*` allow.
- `acl.deny_only` — explicit deny.

## Verification gates

- 6/6 ACL tests pass.
- 45/45 total tests pass.

## Performance

Match is a linear scan over a small pattern list (typically <100 patterns). Cost: ~50 cycles per publish. Same as F14 quota.

## Security

Threats closed:
- **Subject-level access control** — accounts can be limited to specific subject hierarchies.
- **Defense in depth** — deny-list overrides allow-list (explicit revocation wins).

Threats NOT closed:
- **Field-level access control** — current ACL is per-subject, not per-message-field. (Out of scope.)
- **Time-based access control** — no time-of-day rules. (Out of scope.)

## Limitations

- Pattern count is capped at 1024 per list (allow or deny). Production with complex policies may need to compile rules into a more efficient matcher.
- No "imports" / "exports" semantics like NATS. (Future work.)

## See also

- `docs/reviews/hyperplan-v030-plan.md` F16.
- `docs/features/audit.md` — denied publishes trigger audit events.
