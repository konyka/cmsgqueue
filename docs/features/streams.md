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

## Not in this increment

KV / object store and a partition model stay deferred.

## Tests

`tests/test_stream_cursors.c`

## See also

- `docs/reviews/v0.5.56.enumeration.md`
