# F19: Server-Side MQTT 5.0 Listener

## Status

**Shipped.** The full server-side MQTT 3.1.1 / 5.0 listener is
implemented in `src/enterprise/cmq_mqtt_server.{h,c}`. The state
machine handles CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH/PUBACK,
PINGREQ/PINGRESP, and DISCONNECT, plus retain and 5.0 properties.
Topic wildcards `+` and `#` reuse `cmq_sublist`.

This document was originally a STUB spec for v0.4.0; the listener has
since shipped (see git log for `feat(P1): v0.5.x` MQTT commits). The
spec text below is kept for historical reference.

## Configuration

- `cmq_mqtt_set_credentials(user, pass)` — when both non-empty,
  CONNECT must include matching Username/Password.
- `cmq_mqtt_set_listener_enabled(int)` — opt-in toggle; default off.
- `cmq_mqtt_set_bridge_server(srv)` — wire PUBLISH into an existing
  `cmq_server_t`'s sublist so other CMQ clients receive.
- `cmq_mqtt_set_retain_path(path)` — file path for persistent retain;
  retained messages survive restart when set.
- `cmq_mqtt_set_rate_limit(capacity, refill_per_sec)` — token bucket
  per source IP for PUBLISH; default off (0).

## Tests

- `tests/test_mqtt_listen.c` — basic TCP listen success.
- `tests/test_mqtt_qos2.c` — QoS 2 handshake path.
- `tests/test_mqtt_5_wildcard.c` — MQTT 5.0 wildcards `+` and `#`.
- `tests/test_mqtt_retained_file.c` — retain file persistence.
- `tests/test_mqtt_retained_wildcard.c` — retain + wildcards.
- `tests/test_mqtt_bridge*.c` — bridge modes (insert/cleanup/
  freelist_load).

`tests/test_mqtt_will.c` — CONNECT parse, will take-once / fire+retain,
session save/load/drop.

- All `test_mqtt_*` pass; full non-stress ctest at 90/90 PASS.

## See also

- `docs/features/wire-compression.md` — F2 zstd (could be applied to MQTT payloads).
- `docs/features/tls-openssl.md` — F1 TLS (used for mTLS).
- `docs/features/password-hash.md` — F8 scrypt (used for MQTT auth).
- `docs/reviews/hyperplan-v030-plan.md` F19 (original plan, since shipped).
