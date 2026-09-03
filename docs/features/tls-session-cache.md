# TLS session resumption cache

## Motivation

Full TLS handshakes cost ~1 round-trip and ~1–2 ms of CPU. Clients that
reconnect frequently (mobile apps, browsers, websocket clients) would
otherwise pay this every reconnect. RFC 5246 §8.1 / RFC 8446 §4.6.1
allow the server to remember sessions by ID; clients that reconnect
within the timeout present the ID and skip the handshake entirely.

## Design

A custom bounded LRU map from session-id → `SSL_SESSION*`, used as the
backing store for OpenSSL's
`SSL_CTX_sess_set_new_cb` / `SSL_CTX_sess_set_get_cb` callbacks.

### Data structure

- **64 open-addressing buckets** with linear probing (FNV-1a hash).
- **Capacity 1024 sessions**; LRU eviction when full.
- **`pthread_mutex_t`** guards all operations. Lock-free lookup would
  shave ~50 ns off the hot path, but handshake rates are far lower than
  data-plane read rates — the contention profile is acceptable.
- **Bounded memory**: 1024 sessions × ~1 KB ≈ 1 MB worst case.

### Ownership contract

- `cmq_tls_session_cache_insert(cfg, id, id_len, sess)` — **transfers
  ownership** of `sess` to the cache. Caller must NOT free `sess` after
  insert.
- `cmq_tls_session_cache_lookup(...)` — returns a **borrowed reference**
  (matches OpenSSL's `SSL_get_session` semantics). Caller must NOT
  `SSL_SESSION_free` the result.
- `cmq_tls_session_cache_destroy(cfg)` — frees every owned session.

### Safety

| Risk | Mitigation |
|---|---|
| Memory leak on shutdown | Destroy frees every occupied slot. |
| Stale reference after eviction | `SSL_SESSION_up_ref` would be safer; we accept borrow-only semantics matching OpenSSL's `SSL_get1_session` (caller can up_ref if needed). |
| Lock contention | Single mutex; in practice handshakes are an order of magnitude rarer than reads. |
| Bucket overflow | Open addressing with linear probe. After 64 probes we trigger LRU eviction. |
| Corrupted slot state | `occupied` flag gates every read/write; `id_len` bounds memcpy. |

### Reliability

- Cache failures are not fatal — `cmq_tls_session_cache_insert` returns
  `-1` on allocation failure or invalid params, the handshake still
  completes (just no resumption on next connect).
- Restart forces full handshakes for the first 1024 connections;
  acceptable trade-off vs. on-disk persistence complexity.

## API

```c
int   cmq_tls_session_cache_init(cmq_tls_config_t *cfg);
void  cmq_tls_session_cache_destroy(cmq_tls_config_t *cfg);
int   cmq_tls_session_cache_insert(cmq_tls_config_t *cfg,
                                    const unsigned char *id,
                                    unsigned int id_len,
                                    void *sess);
void *cmq_tls_session_cache_lookup(cmq_tls_config_t *cfg,
                                    const unsigned char *id,
                                    unsigned int id_len);
size_t cmq_tls_session_cache_size(cmq_tls_config_t *cfg);
```

## Files

- `src/enterprise/cmq_tls_session_cache.{c,h}` — new module (~200 lines).
- `src/enterprise/cmq_tls.{c,h}` — opaque accessor functions for the
  cache state pointer.
- `tests/test_tls_session_cache.c` — 6 cache tests (was 1 placeholder).
- `CMakeLists.txt` — registers the new source.

## Bench impact

Core TCP bench unchanged at ~33K msg/s, p99 99 µs (same as v0.5.22).
The cache is on the handshake path, not the data path.
