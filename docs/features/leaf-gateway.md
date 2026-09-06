# Leaf and gateway handshake (v0.5.86)

`cmq_leaf_connect` and `cmq_gateway_connect_remote`
dial the configured peer and run `cmq_peer_handshake`
(CONNECT, optional INFO skip, CONNACK code 0).

## Contract

- Leaf `is_connected` is 1 only after CONNECT/CONNACK
  and SUB replay (empty replay is immediate).
- Gateway `connection_count` counts a named live slot
  after the same handshake.
- CONNACK payload byte != 0 fails closed.
- Leftover bytes after CONNACK fail closed.
- Auth caps stay 255 bytes (same as CONNECT).

## Performance

Handshake is on the connect path only. PUBLISH and
route broadcast are unchanged.

## Tests

`tests/test_leafe.c` — loopback hub stub: leaf connect,
gateway connect, bad CONNACK, reject.

`tests/test_cluster.c` remains bookkeeping
(`add_conn(-1)` is not e2e).
`tests/test_cra.c` covers SIGHUP attach of
`cluster_name` / `cluster_node_id` when create had none
(v0.5.143). Existing cluster / route peers are not
recreated.

`tests/test_rta.c` covers SIGHUP attach of `route=`
when create left the live table empty (v0.5.147).
Existing live peers are not redialed.

## See also

- `docs/reviews/v0.5.86.enumeration.md`
- `docs/reviews/v0.5.86.plan.md`
