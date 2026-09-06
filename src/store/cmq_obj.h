#ifndef CMQ_OBJ_H
#define CMQ_OBJ_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_OBJ_NAME_MAX 128
#define CMQ_OBJ_VAL_MAX  65536

typedef struct cmq_obj cmq_obj_t;

cmq_obj_t *cmq_obj_create(const char *dir);
void cmq_obj_destroy(cmq_obj_t *obj);

/* 0 ok; -1 bad name/args; -2 value too large; -3 I/O. */
int cmq_obj_put(cmq_obj_t *obj, const char *name, const uint8_t *data,
                size_t len);
int cmq_obj_get(cmq_obj_t *obj, const char *name, uint8_t *out, size_t out_sz,
                size_t *out_len);
int cmq_obj_del(cmq_obj_t *obj, const char *name);

#endif
