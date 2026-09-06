# F19: Server-Side MQTT Listener

## Status

**Shipped (subset).** CONNECT/CONNACK, PUBLISH QoS 0/1/2 handshake,
SUBSCRIBE/SUBACK, PING, DISCONNECT, retain, topic wildcards, optional
bridge into `cmq_server_publish`. v0.5.43 decodes MQTT 5.0 property
lists (`cmq_mqtt_props_decode`) and computes the PUBLISH payload
offset from the spec layout. Will messages and persistent sessions
remain deferred (R6).

`cmq_mqtt_server_listen` probes bind on `127.0.0.1:1883`. The accept
loop starts only after `cmq_mqtt_set_listener_enabled(1)`.

## MQTT 5.0 properties (v0.5.43)

Property bytes are borrowed from the packet (no malloc). Unknown
identifiers fail closed. 3.1.1 sessions never scan a property VBI.

PUBLISH variable header:

```
topic | [packet_id if QoS>0] | [properties if v5] | payload
```

## Tests

`tests/test_mqtt_props.c` — empty list, content-type, truncated,
unknown id, qos0 3.1.1 offset, qos0/qos1 v5 offset.

## See also

- `docs/reviews/v0.5.43.enumeration.md`
- `docs/features/mqtt-bridge.md`
