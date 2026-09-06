# F19: Server-Side MQTT Listener

## Status

**Shipped (subset).** CONNECT/CONNACK, PUBLISH QoS 0/1/2 handshake,
SUBSCRIBE/SUBACK, PING, DISCONNECT, retain, topic wildcards, optional
bridge into `cmq_server_publish`. v0.5.43 decodes MQTT 5.0 property
lists. v0.5.46 adds last-will (abnormal disconnect only) and
Clean Session durable filters.

`cmq_mqtt_server_listen` probes bind on `127.0.0.1:1883`. The accept
loop starts only after `cmq_mqtt_set_listener_enabled(1)`.

## MQTT 5.0 properties (v0.5.43)

Property bytes are borrowed from the packet (no malloc). Unknown
identifiers fail closed. 3.1.1 sessions never scan a property VBI.

PUBLISH variable header:

```
topic | [packet_id if QoS>0] | [properties if v5] | payload
```

## Last-will and sessions (v0.5.46)

CONNECT flags follow the spec (Will `0x04`, Clean `0x02`,
Username `0x80`, Password `0x40`). A stored will fires on
`recv <= 0`, not on DISCONNECT. Clean Session=0 restores up to
8 filters per client id (32 slots).

## QoS 1 inflight (v0.5.54)

Each live session has a 16-slot outbound window. Matching
local subscribers receive `PUBLISH` with QoS 1 and a packet
id. `PUBACK` frees the slot. A full window skips that dest
(the publisher is not blocked). Payload over 1024 bytes is
not tracked (offer returns `-3`).

## QoS 2 outbound (v0.5.57)

SUBSCRIBE may grant QoS 2. Matching subscribers receive
`PUBLISH` `0x34`. `PUBREC` advances the slot and sends
`PUBREL` `0x62`. `PUBCOMP` frees it. A QoS 1 PUBACK cannot
free a QoS 2 slot; `rec` on a QoS 1 slot fails.

## Tests

`tests/test_mqtt_props.c` — empty list, content-type, truncated,
unknown id, qos0 3.1.1 offset, qos0/qos1 v5 offset.

`tests/test_mqtt_will.c` — CONNECT parse, will take-once / fire+retain,
session save/load/drop.

`tests/test_mqtt_inflight.c` — offer/ack/full/encode, socketpair
fanout + PUBACK, full window skips.

`tests/test_mqtt_qos2.c` — encode 0x34, rec/comp order, PUBREL,
fanout handshake, QoS 1 untouched.

## See also

- `docs/reviews/v0.5.43.enumeration.md`
- `docs/reviews/v0.5.46.enumeration.md`
- `docs/reviews/v0.5.54.enumeration.md`
- `docs/reviews/v0.5.57.enumeration.md`
- `docs/features/mqtt-bridge.md`
