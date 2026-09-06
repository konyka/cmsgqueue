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
/* Cursors: {dir}/js/{name}.cursors. Last: {name}.last.
 * History: {name}.msgs. Parts: {name}.parts */
int cmq_js_set_persist(cmq_js_t *j, const char *dir);
/* 1–16. Empty stream only. 0 ok; -1 bad args / not empty. */
int cmq_js_set_partitions(cmq_js_t *j, const char *name, unsigned n);
unsigned cmq_js_partitions(cmq_js_t *j, const char *name);
/* Default n for new streams without a .parts file. 1–16. */
int cmq_js_set_default_partitions(cmq_js_t *j, unsigned n);
unsigned cmq_js_default_partitions(cmq_js_t *j);
/* 0 = off. After append, rewrite .msgs to a tail that fits cap
 * (and at most the 1024-record ring). */
int cmq_js_set_msgs_rotate_bytes(cmq_js_t *j, uint64_t cap);
uint64_t cmq_js_msgs_rotate_bytes(cmq_js_t *j);
/* v0.5.130: 0 / omitted keeps. parts 1–16; rotate 1–1 GiB.
 * Existing streams keep their .parts file. */
int cmq_js_reload(cmq_js_t *j, int *live_parts, int *live_rotate,
                  int fresh_parts, int fresh_rotate);

/* 0 parsed; -1 not $JS; -2 malformed. */
int cmq_js_parse(const char *subject, char *name, size_t ncap);
/* 0 parsed stream+consumer; -1 not $JS; -2 malformed. */
int cmq_js_parse_cons(const char *subject, char *name, size_t ncap,
                      char *cons, size_t ccap);
/* 0 parsed name+consumer+part (0–15); -1 not $JS; -2 malformed. */
int cmq_js_parse_part(const char *subject, char *name, size_t ncap,
                      char *cons, size_t ccap, unsigned *part);

/* 1 applied; 0 not $JS; -1 malformed / append fail; -2 table full. */
int cmq_js_publish(cmq_js_t *j, const char *subject,
                   const uint8_t *val, size_t len);
int cmq_js_last(cmq_js_t *j, const char *subject, uint8_t *out,
                size_t out_sz, size_t *out_len, uint64_t *out_seq);
/* 1 hit; 0 miss (out_len=0); -1 not $JS / bad args. */
int cmq_js_request(cmq_js_t *j, const char *subject, uint8_t *out,
                   size_t out_sz, size_t *out_len);
/* 1 hit (8-byte BE seq + payload); 0 miss; -1 not consume subject. */
int cmq_js_consume(cmq_js_t *j, const char *subject, uint8_t *out,
                   size_t out_sz, size_t *out_len);
/* 1 hit on that hash partition; 0 miss; -1 bad subject / part. */
int cmq_js_consume_part(cmq_js_t *j, const char *subject, unsigned part,
                        uint8_t *out, size_t out_sz, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
