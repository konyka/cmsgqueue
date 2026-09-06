# F6: MQTT 5.0 Bridge Wired into Server

## Motivation

The `cmq_mqtt_bridge` module existed as a client-only library
(665 LOC) — it could connect OUT to an upstream MQTT broker but
was never wired into the server lifecycle. Operators wanting to
bridge CMQ → MQTT had to run a separate client process.

## Design

A new config field pair `mqtt_bridge_addr` / `mqtt_bridge_port`
(default NULL/0 = disabled). The config file keys are the same
(v0.5.111). Empty addr disables. `/` `\` controls and spaces
are rejected. When set, `cmq_server_create`:

1. Creates an `cmq_mqtt_bridge_t` with a fixed client_id
   `"cmsgbridge"`.
2. Calls `cmq_mqtt_bridge_connect(addr, port)`.
3. On connect failure, logs a warning and disables the bridge
   (server starts anyway — the bridge is best-effort).
4. On success, logs the connection.

On shutdown, `cmq_mqtt_bridge_disconnect` + destroy.

Subject mapping is `mqtt_bridge_map=subject,topic[,qos]`
(repeatable, max 8, v0.5.112) or `cmq_mqtt_add_mapping`.
A matching CMQ PUBLISH is written as MQTT PUBLISH
(`cmq_mqtt_bridge_publish`). Route ingress, WAL replay, and
`mqtt_bridge*` accounts are not re-bridged.
SIGHUP replaces a non-empty map table on the live bridge
(v0.5.126). Omitted maps keep the current table.
v0.5.136: reload applies a non-empty `mqtt_bridge_addr`
and/or non-zero `mqtt_bridge_port` via
`cmq_mqtt_bridge_connect` (same-endpoint live peer is a
no-op). Omitted / empty keeps the current endpoint.
Non-IPv4 and out-of-range port fail closed.
v0.5.142: reload creates and dials the bridge when create
had none. Omitted / empty keeps off. An existing bridge is
left to v0.5.136.

## Files touched

- `src/include/cmq.h` — `mqtt_bridge_addr` / `mqtt_bridge_port` config.
- `src/server/cmq_server.h` — `mqtt_bridge` server field.
- `src/server/cmq_server.c` — create/destroy lifecycle.

## Tests

The existing `test_enterprise.c` covers the MQTT bridge library
(bridge_create_destroy, mapping, topic_conversion, encode/decode
of all message types). No new test added for the wire-up (an
end-to-end test would require a running MQTT broker, out of
scope for this PR).

## Verification gates

- 34/34 tests pass.
- Server starts even when MQTT bridge is unreachable (graceful
  degradation).
- The bridge is enabled only when `mqtt_bridge_addr` is set.

## Performance

The MQTT bridge is a background client. It does not run on the
publish hot path; it consumes from its own queue. No measurable
impact on the 33 K msg/s baseline.

## Security

Threats closed:
- **CMQ → MQTT bridging** — operators no longer need a separate
  client process; the bridge is managed by the server's lifecycle.

Threats NOT closed:
- **Authentication** — the bridge uses no auth. Production should
  add username/password (out of scope; `cmq_mqtt_bridge_connect`
  already supports it but isn't wired here).
- **QoS mapping** — the current bridge uses QoS 0 (at most once).
  QoS 1/2 mapping is a follow-up.

## Limitations

- Forwarding is NOT automatic. Operators must call
  `cmq_mqtt_add_mapping` per subject pattern. A "forward all
  by default" mode is a follow-up.
- The bridge is single-instance. Multi-broker fan-out is a
  follow-up.
- Failure to connect to the upstream does not block server
  startup. Operators must monitor logs.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.6 (F6 catalogued).
- `docs/reviews/round2_deep_attack.md` (MQTT scope critique).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F6.
- `src/enterprise/cmq_mqtt.{c,h}` — library.
