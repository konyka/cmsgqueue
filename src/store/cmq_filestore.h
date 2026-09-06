#ifndef CMQ_FILESTORE_H
#define CMQ_FILESTORE_H

#include <stdint.h>
#include <stddef.h>

typedef struct cmq_filestore cmq_filestore_t;

cmq_filestore_t *cmq_filestore_create(const char *dir, const char *prefix);
void cmq_filestore_destroy(cmq_filestore_t *fs);

int cmq_filestore_append(cmq_filestore_t *fs, const uint8_t *data, size_t len,
                          uint64_t *out_seq);
int cmq_filestore_read(cmq_filestore_t *fs, uint64_t seq,
                          uint8_t **out_data, size_t *out_len);

/* P7: bulk-read a range of records (1..seq_hi - seq_lo + 1). */
typedef struct {
    uint8_t *data;
    size_t len;
} cmq_filestore_range_entry_t;

int cmq_filestore_read_range(cmq_filestore_t *fs, uint64_t seq_lo,
                              uint64_t seq_hi,
                              cmq_filestore_range_entry_t **out_arr,
                              size_t *out_count);
void cmq_filestore_range_free(cmq_filestore_range_entry_t *arr, size_t n);

uint64_t cmq_filestore_last_seq(cmq_filestore_t *fs);
int cmq_filestore_sync(cmq_filestore_t *fs);

/* P2 (v0.5.3): stat counter for async-enqueue blocks. Returns 0 if
 * the filestore has no async ring. */
uint64_t cmq_filestore_async_blocked_count(cmq_filestore_t *fs);
uint64_t cmq_filestore_async_enqueued_count(cmq_filestore_t *fs);

/* P3: install a periodic fsync policy. interval_ms=0 disables
 * periodic fsync (default; only explicit cmq_filestore_sync forces
 * durability). interval_ms>0 calls fdatasync on the data fd every
 * interval_ms milliseconds. */
void cmq_filestore_set_sync_interval(cmq_filestore_t *fs,
                                       unsigned interval_ms);

/* P1: enable async WAL writes via SPSC ring + worker thread.
 * queue_capacity: max in-flight writes (each ~4 KiB avg). 0 = off.
 * Returns 0 on success, -1 on failure (worker thread spawn). */
int cmq_filestore_set_async(cmq_filestore_t *fs, unsigned queue_capacity);

/* v0.5.39: bridge-specific append. Builds a self-describing frame
 * (magic 'CMQB' + version byte + topic_len + topic + payload)
 * and writes it to the FILESTORE via the regular append path. The
 * recovery path (future round) detects this format by the magic and
 * dispatches via cmq_server_publish instead of handle_publish.
 *
 * Returns 0 on success, -1 on error (mirrors cmq_filestore_append).
 * On success, *out_seq is the WAL sequence number. */
int cmq_filestore_append_bridge(cmq_filestore_t *fs,
                                  const char *topic, size_t topic_len,
                                  const uint8_t *payload, size_t payload_len,
                                  uint64_t *out_seq);

/* P1 v0.5.5: cap the per-record payload size accepted by the async
 * enqueue. Default 1 MiB. 0 disables the cap (NOT recommended). */
void cmq_filestore_set_max_payload_size(cmq_filestore_t *fs, size_t bytes);

/* v0.5.45: keep the newest `retain` records (renumbered 1..retain).
 * retain=0 empties the live WAL. No-op if retain >= last_seq. */
int cmq_filestore_compact(cmq_filestore_t *fs, uint64_t retain);

/* v0.5.45: after a sync append, if live .data is >= cap bytes,
 * archive to prefix.data.1 / .idx.1 and start empty. 0 = off. */
void cmq_filestore_set_rotate_bytes(cmq_filestore_t *fs, uint64_t cap);

/* v0.5.53: optional compact key. Payload prefix CMQK + u16le
 * key_len + key + value. Empty value is a tombstone. */
#define CMQ_FS_KEY_MAX 256
int cmq_filestore_key_encode(uint8_t *out, size_t out_sz,
                             const char *key, size_t key_len,
                             const uint8_t *val, size_t val_len,
                             size_t *out_len);
/* 0 if the payload is keyed; -1 otherwise. */
int cmq_filestore_key_decode(const uint8_t *p, size_t n,
                             const uint8_t **key, size_t *key_len,
                             const uint8_t **val, size_t *val_len);
/* Rewrite sealed prefix.data.1 / .idx.1: last value per key,
 * drop tombstones, keep unkeyed. No-op if no archive. Live WAL
 * is not rewritten. */
int cmq_filestore_compact_keys(cmq_filestore_t *fs);

/* P1: enqueue a record for async write. Returns 0 if queued, -1 if
 * queue full / dying / async not enabled. The worker will fwrite +
 * fflush the record; durability follows the fsync policy. */
int cmq_filestore_async_enqueue(cmq_filestore_t *fs, const uint8_t *data,
                                 size_t len, uint64_t seq);

#endif
