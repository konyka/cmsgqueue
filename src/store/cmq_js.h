#ifndef CMQ_JS_H
#define CMQ_JS_H

#include <stddef.h>
#include <stdint.h>

#define CMQ_JS_PREFIX   "$JS."
#define CMQ_JS_NAME_MAX 32
#define CMQ_JS_MAX      8
#define CMQ_JS_VAL_MAX  65536

typedef struct cmq_js cmq_js_t;

#ifdef __cplusplus
extern "C" {
#endif

cmq_js_t *cmq_js_create(void);
void cmq_js_destroy(cmq_js_t *j);
int cmq_js_set_persist(cmq_js_t *j, const char *dir);

/* 0 parsed; -1 not $JS; -2 malformed. */
int cmq_js_parse(const char *subject, char *name, size_t ncap);

/* 1 applied; 0 not $JS; -1 malformed / append fail; -2 table full. */
int cmq_js_publish(cmq_js_t *j, const char *subject,
                   const uint8_t *val, size_t len);
int cmq_js_last(cmq_js_t *j, const char *subject, uint8_t *out,
                size_t out_sz, size_t *out_len, uint64_t *out_seq);
/* 1 hit; 0 miss (out_len=0); -1 not $JS / bad args. */
int cmq_js_request(cmq_js_t *j, const char *subject, uint8_t *out,
                   size_t out_sz, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
