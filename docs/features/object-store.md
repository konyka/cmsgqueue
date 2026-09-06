# Object store (v0.5.59, D4 phase 3)

`cmq_obj` stores one file per name under a directory. Put is
tmp + fsync + rename. Get is a single read. Delete unlinks.

## Limits

- Names: `[A-Za-z0-9._-]`. `.` / `..` / `/` rejected.
- Directory must not contain `..` or `\`.
- Value cap 64 KiB this version.

Missing names are a miss. A failed put leaves the previous
file.

Library API only — not wired into the server process.

## Tests

`tests/test_obj.c`

## See also

- `docs/reviews/v0.5.59.enumeration.md`
