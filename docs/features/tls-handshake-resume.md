# TLS handshake resume across event-loop wakeups

**Status:** shipped in v0.5.45

## Problem

`cmq_tls_handshake()` is non-blocking — it returns 1 (done), 0
(WANT_READ/WANT_WRITE), or -1 (fatal error). A real TLS handshake
needs multiple TCP round-trips before it's done, so `rc == 0` is
the common case during the first few wakeups.

`client_tls_handshake()` (the wrapper called from `accept_cb`) and
`client_read_cb()` (the per-client EV_READ handler) must together
drive the OpenSSL state machine until `handshake_done` flips.

## What v0.5.45 shipped

### `client_tls_handshake` (cmq_server.c)

- Old: `if (rc != 0) { destroy; return -1; }` — killed successful
  handshakes because `cmq_tls_handshake` returns 1 on success.
- New: only `rc < 0` (real error) destroys and rejects. `rc == 0`
  (pending) and `rc == 1` (success) both keep the session alive
  and attach it to `client->tls`.

### `client_read_cb` (cmq_server.c)

Before calling `client_sock_read()` (which delegates to
`cmq_tls_read` when `c->tls` is set), check
`cmq_tls_handshake_done(c->tls)`. If not done:

```c
if (c->tls && !cmq_tls_handshake_done(c->tls)) {
    int hrc = cmq_tls_handshake(c->tls);
    if (hrc < 0) { client_teardown(c); return; }
    if (hrc == 0) return;  /* still pending; wait for next wakeup */
    /* rc == 1 → fall through to read application data. */
}
```

This resumes the handshake on every EV_READ until complete, then
proceeds with the normal protocol parser.

### `cmq_tls_handshake_done` (cmq_tls.{c,h})

New accessor exposing the `handshake_done` field of
`cmq_tls_session_t`. Returns 1 if done, 0 if pending, -1 on bad
input. Lets server callbacks query handshake state without
exposing the struct layout.

## Why this was needed

Before v0.5.45, every TLS connection through `cmq_server_run`
either:

- was rejected on accept (if `cmq_tls_handshake` returned 1 — a
  successful synchronous handshake), or
- was accepted with `client->tls` set but the state machine
  frozen (any handshake needing WANT_READ/WANT_WRITE).

The bug was masked because all prior TLS tests either
bypassed `cmq_server` entirely (v0.5.28's socketpair test) or
never completed the handshake (v0.5.33's
`per_listener_tls_accepts_connection` opens a TCP socket then
closes it).

## Test coverage

`tests/test_tls_e2e_handshake.c` ships two tests that exercise
the full handshake path through `cmq_server_run`:

1. **single_listener** — one TLS listener, drives a real
   `SSL_connect` from the client side, asserts the handshake
   completes.
2. **multi_listener_distinct_certs** — two listeners with
   distinct self-signed certs. Slot 0 on port N trusts cert0;
   slot 1 on port N+1 trusts cert1. A cross-check (connect to
   slot 1's port but trust cert0) is rejected by the client,
   proving the per-listener slot lookup reaches the actual
   handshake in production.

The second test is the first end-to-end proof that v0.5.33's
`srv_find_tls_slot` is wired correctly all the way through to
the bytes the server sends on the wire.
