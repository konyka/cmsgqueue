#ifndef CMQ_IDEMPO_H
#define CMQ_IDEMPO_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_IDEMPO_MAGIC "CMQI"
#define CMQ_IDEMPO_HDR_LEN 16
#define CMQ_IDEMPO_PIDS 256
#define CMQ_IDEMPO_WIN 64

typedef struct cmq_idempo cmq_idempo_t;

cmq_idempo_t *cmq_idempo_create(void);
void cmq_idempo_destroy(cmq_idempo_t *t);

int cmq_idempo_encode(uint8_t *out, size_t cap, uint32_t pid, uint64_t seq,
                      size_t *out_len);
int cmq_idempo_parse(const uint8_t *hdr, size_t n, uint32_t *pid,
                     uint64_t *seq);

/* 1 = new; 0 = duplicate / too old; -1 = bad args; -2 = table full. */
int cmq_idempo_check(cmq_idempo_t *t, uint32_t pid, uint64_t seq);

#endif
