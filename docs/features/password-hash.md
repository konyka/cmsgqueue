# Password Hashing (scrypt)

CMSGQueue stores password credentials as scrypt hashes using the existing
`cmq_password_hash()` and `cmq_password_verify()` APIs. The wire format and
OpenSSL parameters are unchanged: `N=16384`, `r=8`, `p=1`, a random 16-byte
salt, and a 32-byte derived key.

## `cmq-password`

The `cmq-password` tool generates a hash for configuring an authentication
password. It reads only from standard input and refuses a terminal, so a
password is never placed in command-line arguments or shell history:

```sh
cmq-password < protected-password-file
```

Input rules:

- The password is 1 to 255 bytes after line-ending handling.
- One final LF is stripped; one CR immediately before that LF is stripped as
  part of CRLF handling.
- Embedded NUL, CR, LF, empty input, additional lines, and input over 255
  bytes are rejected. Input without a final newline is valid.
- The input file or pipe should be protected from other users. Do not pass a
  password as an argument or place it in an ordinary shell command line.

On success, stdout contains the scrypt hash followed by exactly one LF and the
process exits zero. Errors produce no hash and a nonzero exit status. Each
invocation generates a fresh random salt, so identical passwords normally
produce different hashes.

Use `cmq-password --help` for the short usage line. The tool is built under
`tools/` independently of the optional example programs, so it is available
with `-DCMQ_BUILD_EXAMPLES=OFF` on POSIX builds.

## Library API

- `cmq_password_hash(password, out, out_len)` generates the formatted hash.
- `cmq_password_verify(stored, password)` returns `1` for a match, `0` for a
  mismatch, and `-1` for malformed stored data.

The server continues to accept legacy plaintext configuration during
transition, but new deployments should use the generated scrypt format.

## Verification

The password unit tests cover hash round trips, random salts, malformed hashes,
buffer limits, and empty-password rejection. `tests/test_password_cli.c`
covers the CLI input contract, boundary lengths, newline handling, argument
rejection, and help output.
