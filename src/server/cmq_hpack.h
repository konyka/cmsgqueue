#ifndef CMQ_HPACK_H
#define CMQ_HPACK_H

#include <stddef.h>
#include <stdint.h>

#define CMQ_HPACK_STATIC_MAX 61
#define CMQ_HPACK_INT_MAX    (1u << 20)
#define CMQ_HPACK_STR_MAX    256
#define CMQ_HPACK_DYN_MAX    4096
#define CMQ_HPACK_DYN_SLOTS  128

typedef struct {
    uint16_t noff;
    uint16_t nlen;
    uint16_t voff;
    uint16_t vlen;
} cmq_hpack_dent_t;

typedef struct {
    uint8_t buf[CMQ_HPACK_DYN_MAX];
    cmq_hpack_dent_t e[CMQ_HPACK_DYN_SLOTS];
    unsigned n;
    unsigned size;
    unsigned max;
    unsigned used;
} cmq_hpack_dyn_t;

#ifdef __cplusplus
extern "C" {
#endif

int cmq_hpack_int_encode(uint32_t value, unsigned nbits, uint8_t prefix_hi,
                         uint8_t *out, size_t cap);
int cmq_hpack_int_decode(const uint8_t *in, size_t in_len, unsigned nbits,
                         uint32_t *value, size_t *consumed);

int cmq_hpack_str_encode(const char *s, uint8_t *out, size_t cap);
int cmq_hpack_str_decode(const uint8_t *in, size_t in_len, char *out,
                         size_t out_cap, size_t *consumed);
int cmq_hpack_huff_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t cap);
int cmq_hpack_huff_decode(const uint8_t *in, size_t in_len, char *out,
                          size_t cap);

int cmq_hpack_static_get(unsigned idx, const char **name,
                         const char **value);

int cmq_hpack_hdr_encode_indexed(unsigned idx, uint8_t *out, size_t cap);
int cmq_hpack_hdr_decode(const uint8_t *in, size_t in_len, char *name,
                         size_t ncap, char *value, size_t vcap,
                         size_t *consumed);

void cmq_hpack_dyn_init(cmq_hpack_dyn_t *t);
int cmq_hpack_dyn_set_max(cmq_hpack_dyn_t *t, unsigned max);
unsigned cmq_hpack_dyn_size(const cmq_hpack_dyn_t *t);
unsigned cmq_hpack_dyn_count(const cmq_hpack_dyn_t *t);
int cmq_hpack_dyn_add(cmq_hpack_dyn_t *t, const char *name, const char *value);
int cmq_hpack_dyn_get(const cmq_hpack_dyn_t *t, unsigned newest_off,
                      const char **name, size_t *nlen, const char **value,
                      size_t *vlen);
int cmq_hpack_hdr_encode_inc(cmq_hpack_dyn_t *t, const char *name,
                             const char *value, uint8_t *out, size_t cap);
/* 0 = header, 1 = table size update, -1 = fail. */
int cmq_hpack_hdr_decode_dyn(cmq_hpack_dyn_t *t, const uint8_t *in,
                             size_t in_len, char *name, size_t ncap,
                             char *value, size_t vcap, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif
