#define _POSIX_C_SOURCE 200809L
#include "cmq_obj.h"
#include "cmq_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

struct cmq_obj {
    char dir[512];
    cmq_mutex_t lock;
};

static int obj_name_safe(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '.' && s[1] == '.' && s[2] == '\0') return 0;
    if (s[0] == '.' && s[1] == '\0') return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, n++) {
        unsigned char c = *p;
        if (n >= CMQ_OBJ_NAME_MAX) return 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static int obj_dir_safe(const char *dir) {
    if (!dir || !dir[0]) return 0;
    size_t n = strnlen(dir, 512);
    if (n == 0 || n >= 512) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)dir[i];
        if (c == '\\' || c < 0x20 || c == 0x7f) return 0;
    }
    size_t i = 0;
    while (i < n) {
        while (i < n && dir[i] == '/') i++;
        if (i >= n) break;
        size_t start = i;
        while (i < n && dir[i] != '/') i++;
        size_t len = i - start;
        if (len == 1 && dir[start] == '.') return 0;
        if (len == 2 && dir[start] == '.' && dir[start + 1] == '.') return 0;
    }
    return 1;
}

static int obj_path(const cmq_obj_t *obj, const char *name, char *out,
                    size_t out_sz, const char *suffix) {
    int n = snprintf(out, out_sz, "%s/%s%s", obj->dir, name,
                     suffix ? suffix : "");
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

cmq_obj_t *cmq_obj_create(const char *dir) {
    if (!obj_dir_safe(dir)) return NULL;
    cmq_obj_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    snprintf(o->dir, sizeof(o->dir), "%s", dir);
    cmq_mutex_init(&o->lock);
    return o;
}

void cmq_obj_destroy(cmq_obj_t *obj) {
    if (!obj) return;
    cmq_mutex_destroy(&obj->lock);
    free(obj);
}

int cmq_obj_put(cmq_obj_t *obj, const char *name, const uint8_t *data,
                size_t len) {
    if (!obj || !obj_name_safe(name) || (len > 0 && !data)) return -1;
    if (len > CMQ_OBJ_VAL_MAX) return -2;
    char path[640], tmp[648];
    if (obj_path(obj, name, path, sizeof(path), NULL) != 0 ||
        obj_path(obj, name, tmp, sizeof(tmp), ".tmp") != 0)
        return -1;
    cmq_mutex_lock(&obj->lock);
    int rc = -3;
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        int ok = 1;
        if (len > 0)
            ok = (fwrite(data, 1, len, fp) == len);
        if (ok) ok = (fflush(fp) == 0 && fsync(fileno(fp)) == 0);
        fclose(fp);
        if (!ok) {
            unlink(tmp);
        } else if (rename(tmp, path) != 0) {
            unlink(tmp);
        } else {
            rc = 0;
        }
    }
    cmq_mutex_unlock(&obj->lock);
    return rc;
}

int cmq_obj_get(cmq_obj_t *obj, const char *name, uint8_t *out, size_t out_sz,
                size_t *out_len) {
    if (!obj || !obj_name_safe(name) || !out || !out_len) return -1;
    char path[640];
    if (obj_path(obj, name, path, sizeof(path), NULL) != 0) return -1;
    cmq_mutex_lock(&obj->lock);
    int rc = -1;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long sz = ftell(fp);
            if (sz >= 0 && (size_t)sz <= CMQ_OBJ_VAL_MAX &&
                (size_t)sz <= out_sz && fseek(fp, 0, SEEK_SET) == 0) {
                if (sz == 0 || fread(out, 1, (size_t)sz, fp) == (size_t)sz) {
                    *out_len = (size_t)sz;
                    rc = 0;
                }
            } else if (sz > (long)out_sz) {
                rc = -2;
            } else {
                rc = -3;
            }
        }
        fclose(fp);
    }
    cmq_mutex_unlock(&obj->lock);
    return rc;
}

int cmq_obj_parse(const char *subject, char *name, size_t ncap) {
    if (!subject) return -1;
    if (subject[0] != '$') return -1;
    if (strncmp(subject, CMQ_OBJ_PREFIX, 5) != 0) return -1;
    const char *rest = subject + 5;
    if (!obj_name_safe(rest)) return -2;
    if (name && ncap) {
        size_t n = strlen(rest);
        if (n >= ncap) return -2;
        memcpy(name, rest, n + 1);
    }
    return 0;
}

int cmq_obj_publish(cmq_obj_t *obj, const char *subject,
                    const uint8_t *val, size_t len) {
    if (!obj) return -1;
    char name[CMQ_OBJ_NAME_MAX];
    int pr = cmq_obj_parse(subject, name, sizeof(name));
    if (pr == -1) return 0;
    if (pr != 0) return -1;
    if (len == 0) {
        (void)cmq_obj_del(obj, name);
        return 1;
    }
    int p = cmq_obj_put(obj, name, val, len);
    if (p == 0) return 1;
    if (p == -2) return -2;
    if (p == -3) return -3;
    return -1;
}

int cmq_obj_del(cmq_obj_t *obj, const char *name) {
    if (!obj || !obj_name_safe(name)) return -1;
    char path[640];
    if (obj_path(obj, name, path, sizeof(path), NULL) != 0) return -1;
    cmq_mutex_lock(&obj->lock);
    int rc = (unlink(path) == 0) ? 0 : -1;
    cmq_mutex_unlock(&obj->lock);
    return rc;
}
