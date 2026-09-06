# Stream consumer cursors (v0.5.56, D4 phase 1)

`cmq_stream` already keeps in-memory consumers and cumulative
ack watermarks. Those die with `cmq_stream_destroy`. This
increment persists the watermarks so a reopen resumes the
same sequence.

## Opt-in

`cmq_stream_create` does no I/O. Call
`cmq_stream_set_cursor_path(stream, dir)` to enable
`{dir}/{name}.cursors`. Missing file is empty.

Append / read stay on the in-memory ring. Persist runs only
on add / ack / remove (tmp + fsync + rename).

## File format

```
CMQC1
worker1 2
worker2 0
```

Empty value is not a tombstone — remove the consumer so the
name is omitted on the next write.

## Safety

- Directory must not contain `..`, `.`, or `\`.
- Durable consumer names are `[A-Za-z0-9._-]`.
- Memory-only streams still accept any existing name.

Message bodies are **not** durable here. After destroy the
ring resets to seq 1; only the cursor file is reloaded.

## Partitions (v0.5.87)

Default `nparts=1`: `append` / `next` / `ack` unchanged.
`cmq_stream_set_partitions(n)` (1–16) is allowed only while
the stream is empty. `append_key` stores FNV-1a `% n` in a
`max_msgs` sidecar. `next_part` / `ack_part` isolate
watermarks. Wrong-part ack fails closed.

Cursor file is `CMQC2` when n>1 (`name part seq`); `CMQC1`
is unchanged.

## Stream PUBLISH path (v0.5.93)

PUBLISH `$JS.<name>` appends the payload to a named
in-memory stream (`cmq_js`). Non-`$` subjects pay the
existing one-byte compare. At most 8 lazy streams.
Empty payload fails closed (the ring refuses len 0).
Fanout still runs so watchers see the write.

Names: `[A-Za-z0-9_-]`, max 32, no dots.

Optional cursors: `{persist_dir}/js/{name}.cursors`
when `persist_dir` is set. The last payload is also
written to `{persist_dir}/js/{name}.last` (`CMQL`,
v0.5.103) so REQUEST-get survives reopen. History
is appended to `{persist_dir}/js/{name}.msgs`
(`CMQM`, v0.5.104) and replayed onto the ring on
open so pull consume survives reopen.

REQUEST `$JS.<name>` (v0.5.94) returns the last payload
to reply-to, or an empty body on miss. Oversized last
message is an empty miss.

## Consume / ack (v0.5.95)

`$JS.<name>.<consumer>` is a pull consumer. Both tokens
use the same alphabet as the stream name.

REQUEST copies the next message with an 8-byte big-endian
seq prefix. The watermark does not move. Miss is empty.
First pull adds the consumer.

PUBLISH with exactly 8 BE seq bytes acks. Unknown stream
or consumer fails closed (does not append).

## Tests

`tests/test_stream_cursors.c`, `tests/test_spart.c`,
`tests/test_js.c`, `tests/test_jsr.c`, `tests/test_jsc.c`,
`tests/test_jsl.c`, `tests/test_jsh.c`

## See also

- `docs/reviews/v0.5.56.enumeration.md`
- `docs/reviews/v0.5.87.enumeration.md`
- `docs/reviews/v0.5.93.enumeration.md`
- `docs/reviews/v0.5.94.enumeration.md`
- `docs/reviews/v0.5.95.enumeration.md`
- `docs/reviews/v0.5.103.enumeration.md`
- `docs/reviews/v0.5.104.enumeration.md`
