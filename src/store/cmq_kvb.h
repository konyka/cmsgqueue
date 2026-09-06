#ifndef CMQ_KVB_H
#define CMQ_KVB_H

#include <stddef.h>
#include <stdint.h>

#define CMQ_KVB_PREFIX     "$KV."
#define CMQ_KVB_BUCKET_MAX 32
#define CMQ_KVB_MAX        8

typedef struct cmq_kvb cmq_kvb_t;

#ifdef __cplusplus
extern "C" {
#endif

cmq_kvb_t *cmq_kvb_create(void);
void cmq_kvb_destroy(cmq_kvb_t *b);
int cmq_kvb_set_persist(cmq_kvb_t *b, const char *dir);

/* 0 parsed; -1 not $KV; -2 malformed. */
int cmq_kvb_parse(const char *subject, char *bucket, size_t bcap,
                  char *key, size_t kcap);

/* 1 applied; 0 not KV; -1 malformed; -2 full; -3 too large. */
int cmq_kvb_publish(cmq_kvb_t *b, const char *subject,
                    const uint8_t *val, size_t len);
int cmq_kvb_get(cmq_kvb_t *b, const char *subject, uint8_t *out,
                size_t out_sz, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
