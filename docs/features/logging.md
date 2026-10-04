# Logger (v0.5.127, v0.5.128, v0.5.168)

`cmq_log` is an async multi-appender logger. Create
seeds the level from `log_level` (0=TRACE … 5=FATAL;
out of range falls back to INFO).

## Sinks

`log_to_stdout`, `log_to_file`, and `log_file` add
appenders at create. SIGHUP applies them through
`cmq_log_reload_sinks` (v0.5.127): stdout is added
only if missing; a new file path replaces the file
appender after in-flight writers finish. 0 / omitted /
empty path keeps the current sinks. `fopen` failure
fail-closes and keeps the previous file.

Omitted `log_to_stdout` defaults to 1 (v0.5.128).

## Attach

Create does not fail if `cmq_log_create` returns NULL.
v0.5.168: SIGHUP creates the logger when create left
`log` NULL so sink reload does not abort the apply.
An existing logger is not remounted. Level on a just
created handle comes from the live `log_level`
(clamped to INFO if out of range). `apply_dynamic`
still sets the level from the fresh file.

## Tests

`tests/test_log.c`, `tests/test_lsk.c`, `tests/test_laa.c`

## See also

- `docs/reviews/v0.5.127.enumeration.md`
- `docs/reviews/v0.5.128.enumeration.md`
- `docs/reviews/v0.5.168.enumeration.md`
