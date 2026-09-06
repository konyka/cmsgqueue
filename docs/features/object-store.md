# Object store (v0.5.59–68, D4 phases 3 and 5)

`cmq_obj` stores one file per name under a directory. Put is
tmp + fsync + rename. Get is a single read. Delete unlinks.

## Limits

- Names: `[A-Za-z0-9._-]`. `.` / `..` / `/` rejected.
- Directory must not contain `..` or `\`.
- Value cap 64 KiB this version.

Missing names are a miss. A failed put leaves the previous
file.

## PUBLISH path (v0.5.68)

When `persist_dir` is set, the server opens `{persist_dir}/obj`
and applies `$OBJ.<name>` on PUBLISH: non-empty payload puts,
empty payload deletes. Fanout still runs. Without
`persist_dir`, `$OBJ.` is a normal subject. REQUEST-get stays
deferred.

## Tests

`tests/test_obj.c`, `tests/test_objp.c`

## See also

- `docs/reviews/v0.5.59.enumeration.md`
- `docs/reviews/v0.5.68.enumeration.md`
