#ifndef CMQ_KV_H
#define CMQ_KV_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_KV_KEY_MAX 256
#define CMQ_KV_VAL_MAX 1024
#define CMQ_KV_SLOTS_MAX 256

typedef struct cmq_kv cmq_kv_t;

/* Memory-only. slots==0 uses CMQ_KV_SLOTS_MAX. */
cmq_kv_t *cmq_kv_create(size_t slots);
void cmq_kv_destroy(cmq_kv_t *kv);

/* Open/replay live WAL at dir/prefix. Missing files are empty. */
int cmq_kv_set_persist(cmq_kv_t *kv, const char *dir, const char *prefix);

/* 0 ok; -1 bad key/args; -2 table full (new key); -3 value too large. */
int cmq_kv_put(cmq_kv_t *kv, const char *key, const uint8_t *val, size_t len);
int cmq_kv_get(cmq_kv_t *kv, const char *key, uint8_t *out, size_t out_sz,
               size_t *out_len);
int cmq_kv_del(cmq_kv_t *kv, const char *key);
size_t cmq_kv_count(cmq_kv_t *kv);

#endif
