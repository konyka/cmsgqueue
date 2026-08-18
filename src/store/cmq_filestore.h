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

/* P1: enqueue a record for async write. Returns 0 if queued, -1 if
 * queue full / dying / async not enabled. The worker will fwrite +
 * fflush the record; durability follows the fsync policy. */
int cmq_filestore_async_enqueue(cmq_filestore_t *fs, const uint8_t *data,
                                 size_t len, uint64_t seq);

#endif
