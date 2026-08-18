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

#endif
