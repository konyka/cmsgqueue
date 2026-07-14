#ifndef CMQ_STORE_H
#define CMQ_STORE_H

#include <stdint.h>
#include <stddef.h>

typedef struct cmq_store cmq_store_t;

typedef struct {
    uint64_t seq;
    uint8_t *data;
    size_t len;
    uint64_t timestamp_ms;
} cmq_store_msg_t;

cmq_store_t *cmq_store_create(size_t capacity);
void cmq_store_destroy(cmq_store_t *store);

uint64_t cmq_store_put(cmq_store_t *store, const uint8_t *data, size_t len);
/* Takes ownership of data (freed on failure or when slot is overwritten). */
uint64_t cmq_store_put_owned(cmq_store_t *store, uint8_t *data, size_t len);
int cmq_store_get(cmq_store_t *store, uint64_t seq, cmq_store_msg_t *out);
/* Length of seq without allocating a copy. Returns 0 on success, -1 if missing. */
int cmq_store_seq_len(cmq_store_t *store, uint64_t seq, size_t *out_len);
void cmq_store_msg_release(cmq_store_msg_t *msg); /* free copy from get */
size_t cmq_store_count(cmq_store_t *store);
uint64_t cmq_store_first_seq(cmq_store_t *store);
uint64_t cmq_store_last_seq(cmq_store_t *store);
void cmq_store_truncate(cmq_store_t *store, uint64_t before_seq);
/* O(1) invalidate the oldest sequence only (prefix eviction; keeps first_seq
   continuous). Returns 0 on success. */
int cmq_store_evict_seq(cmq_store_t *store, uint64_t seq);

#endif
