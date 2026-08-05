# F19: Server-Side MQTT 5.0 Listener (STUB)

## Status

**STUB.** The full server-side MQTT 5.0 listener is deferred to v0.4.0.

The MQTT 3.1.1 / 5.0 protocol is a full state machine (CONNECT/CONNACK/SUBSCRIBE/SUBACK/PUBLISH/PUBACK/PINGREQ/PINGRESP/DISCONNECT). Implementing this requires a real MQTT parser, session state, and protocol compliance. CMSGQueue has the `cmq_mqtt` library (encoder-only for client + decoder for connack/packet_type). A server-side listener would re-implement mosquitto or NanoMQ at the protocol level.

## Design (deferred)

The plan:
1. **MQTT 3.1.1** — minimal broker, QoS 0/1 only.
2. **MQTT 5.0** — properties, reason codes, subscription options, shared subscriptions.
3. **Sessions** — clean session vs persistent session, message queue per client.
4. **Auth** — username/password (reuses F8 scrypt), mTLS via F1.
5. **Topic wildcards** — already implemented in `cmq_sublist`.

Estimate: **XL (2-4 weeks for one engineer)** to land a protocol-compliant broker.

## Files touched (stub only)

- `src/enterprise/cmq_mqtt_server.{h,c}` (new) — stub returning `ENOSYS`.
- `tests/test_mqtt_server_stub.c` (new) — verifies stub behavior.

## Tests

`tests/test_mqtt_server_stub.c`:
- `mqtt_server.listen_returns_enosys` — `cmq_mqtt_server_listen` returns `-ENOSYS`.
- `mqtt_server.listen_with_null_addr_returns_enosys` — null-arg path.

## Verification gates

- 2/2 stub tests pass.
- 45/45 total tests pass.

## See also

- `docs/features/wire-compression.md` — F2 zstd (could be applied to MQTT payloads).
- `docs/features/tls-openssl.md` — F1 TLS (used for mTLS).
- `docs/features/password-hash.md` — F8 scrypt (used for MQTT auth).
- `docs/reviews/hyperplan-v030-plan.md` F19.
