#ifndef CMQ_HPACK_H
#define CMQ_HPACK_H

#include <stddef.h>
#include <stdint.h>

#define CMQ_HPACK_STATIC_MAX 61
#define CMQ_HPACK_INT_MAX    (1u << 20)
#define CMQ_HPACK_STR_MAX    256

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

#ifdef __cplusplus
}
#endif

#endif
