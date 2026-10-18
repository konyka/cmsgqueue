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
2. Calls the credential-capable bridge connect API with the optional
   `mqtt_bridge_username` and `mqtt_bridge_password` fields. Empty or omitted
   credentials leave the MQTT CONNECT packet unauthenticated. The password is
   never logged.
3. On connect failure, logs a warning and disables the bridge
   (server starts anyway — the bridge is best-effort).
4. On success, logs the connection.

On shutdown, `cmq_mqtt_bridge_disconnect` + destroy.

Subject mapping is `mqtt_bridge_map=subject,topic[,qos]`
(repeatable, max 8, v0.5.112) or `cmq_mqtt_add_mapping`.
A matching CMQ PUBLISH is written as MQTT PUBLISH
(`cmq_mqtt_bridge_publish`). Route ingress, WAL replay, and
`mqtt_bridge*` accounts are not re-bridged.
Unmapped subjects are not forwarded by default. Set
`mqtt_bridge_forward_all = 1` to forward unmapped subjects after the
standard `cmq_mqtt_subject_to_topic` conversion (`.` to `/`). This option
is default-off and is applied on SIGHUP using the same reload rules as the
other bridge settings. Subjects containing CMQ or MQTT wildcard characters
(`*`, `>`, `+`, `#`) and conversions that cannot fit the MQTT topic buffer
are rejected without forwarding. Mapped subjects always use their explicit
topic and QoS.
QoS 0 publishes complete after the write; QoS 1 publishes wait for a
matching PUBACK before reporting success. A malformed or mismatched QoS 1
PUBACK is a publish failure: the server increments its rejected-publish
metric and reports an error to normal CMQ clients, while local fanout still
continues. Non-client bridge relay publishes return failure to their caller
under the same condition. QoS 2 completes the PUBREC/PUBREL/PUBCOMP handshake.
SIGHUP replaces a non-empty map table on the live bridge
(v0.5.126). Omitted maps keep the current table.
v0.5.156: SIGHUP also copies that table onto the live
config. Count 0 / omitted keeps the current rows.
`..` / `\` / empty subject or topic / qos outside 0–2
fail closed.
v0.5.136: reload applies a non-empty `mqtt_bridge_addr`
and/or non-zero `mqtt_bridge_port` via
`cmq_mqtt_bridge_connect` (same-endpoint live peer is a
no-op). Omitted / empty keeps the current endpoint.
Non-IPv4 and out-of-range port fail closed.
v0.5.142: reload creates and dials the bridge when create
had none. Omitted / empty keeps off. An existing bridge is
left to v0.5.136. Reload also applies non-omitted username/password values;
changing credentials reconnects an existing bridge at the same endpoint.
Omitted or empty credential keys retain the live values.

Bridge authentication keys:

```ini
mqtt_bridge_username = bridge-user
mqtt_bridge_password = bridge-secret
```

Both values are optional and are owned by the loaded/server configuration.
The bridge copies them for CONNECT setup and cleanses its stored copies on
destroy. SIGHUP reloads replace the live values only after a successful
reconnect. Do not put credentials in logs.

## Files touched

- `src/include/cmq.h` — bridge endpoint and optional authentication config.
- `src/server/cmq_server.h` — `mqtt_bridge` server field.
- `src/server/cmq_server.c` — create/destroy lifecycle.

## Tests

`test_mqb.c` covers parser fields/defaults, `test_mqp.c` covers the
credential-bearing MQTT CONNECT packet, and `test_mqe.c` covers credential
rotation during endpoint reload. `test_mqa.c` covers reload validation. The
existing `test_enterprise.c` also covers the MQTT bridge library
(bridge_create_destroy, mapping, topic_conversion, encode/decode
of all message types). Mock-broker tests check the CONNECT wire payload
without requiring an external MQTT service.

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
- **Authentication** — username/password authentication is supported for
  initial connect and reload. Credentials are not logged and are cleared from
  bridge/config buffers when released.
- **QoS mapping** — QoS 0, QoS 1, and QoS 2 mappings are supported.

## Limitations

- Forwarding remains mapping-driven by default; set
  `mqtt_bridge_forward_all = 1` for unmapped subjects. Wildcards and invalid
  conversions are rejected. The bridge is single-instance. Multi-broker
  fan-out is a follow-up.
- Failure to connect to the upstream does not block server
  startup. Operators must monitor logs.

## See also

- `docs/reviews/hyperplan-bundle.md` §1.6 (F6 catalogued).
- `docs/reviews/round2_deep_attack.md` (MQTT scope critique).
- Plan reference: `docs/reviews/hyperplan-final-plan.md` Part 2, F6.
- `src/enterprise/cmq_mqtt.{c,h}` — library.
