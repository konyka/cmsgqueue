#ifndef CMQ_OBJ_H
#define CMQ_OBJ_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_OBJ_NAME_MAX 128
#define CMQ_OBJ_VAL_MAX  65536
#define CMQ_OBJ_PREFIX   "$OBJ."

typedef struct cmq_obj cmq_obj_t;

cmq_obj_t *cmq_obj_create(const char *dir);
void cmq_obj_destroy(cmq_obj_t *obj);

/* 0 ok; -1 bad name/args; -2 value too large; -3 I/O. */
int cmq_obj_put(cmq_obj_t *obj, const char *name, const uint8_t *data,
                size_t len);
int cmq_obj_get(cmq_obj_t *obj, const char *name, uint8_t *out, size_t out_sz,
                size_t *out_len);
int cmq_obj_del(cmq_obj_t *obj, const char *name);

/* 0 parsed; -1 not $OBJ; -2 malformed. */
int cmq_obj_parse(const char *subject, char *name, size_t ncap);
/* 1 applied; 0 not $OBJ; -1 malformed; -2 too large; -3 I/O. */
int cmq_obj_publish(cmq_obj_t *obj, const char *subject,
                    const uint8_t *val, size_t len);
/* 1 hit; 0 miss (out_len=0); -1 not $OBJ / bad args. */
int cmq_obj_request(cmq_obj_t *obj, const char *subject, uint8_t *out,
                    size_t out_sz, size_t *out_len);

#endif
