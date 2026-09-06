#define _POSIX_C_SOURCE 200809L
#include "cmq_js.h"
#include "cmq_stream.h"
#include "cmq_thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char name[CMQ_JS_NAME_MAX];
    cmq_stream_t *st;
} cmq_js_slot_t;

struct cmq_js {
    cmq_js_slot_t slots[CMQ_JS_MAX];
    int n;
    char persist_dir[512];
    cmq_mutex_t lock;
};

static int name_ok(const char *s, size_t n) {
    if (n == 0 || n >= CMQ_JS_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

int cmq_js_parse(const char *subject, char *name, size_t ncap) {
    if (!subject) return -1;
    if (subject[0] != '$') return -1;
    if (strncmp(subject, CMQ_JS_PREFIX, 4) != 0) return -1;
    const char *rest = subject + 4;
    if (!rest[0] || strchr(rest, '.')) return -2;
    size_t n = strlen(rest);
    if (!name_ok(rest, n)) return -2;
    if (name && ncap) {
        if (n >= ncap) return -2;
        memcpy(name, rest, n + 1);
    }
    return 0;
}

cmq_js_t *cmq_js_create(void) {
    cmq_js_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    cmq_mutex_init(&j->lock);
    return j;
}

void cmq_js_destroy(cmq_js_t *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++)
        cmq_stream_destroy(j->slots[i].st);
    cmq_mutex_destroy(&j->lock);
    free(j);
}

int cmq_js_set_persist(cmq_js_t *j, const char *dir) {
    if (!j || !dir || !dir[0]) return -1;
    size_t n = strnlen(dir, 480);
    if (n == 0 || n >= 480) return -1;
    char jsdir[512];
    int w = snprintf(jsdir, sizeof(jsdir), "%s/js", dir);
    if (w < 0 || (size_t)w >= sizeof(jsdir)) return -1;
    if (mkdir(jsdir, 0755) != 0 && errno != EEXIST)
        return -1;
    cmq_mutex_lock(&j->lock);
    memcpy(j->persist_dir, jsdir, (size_t)w + 1);
    int rc = 0;
    for (int i = 0; i < j->n; i++) {
        if (cmq_stream_set_cursor_path(j->slots[i].st, j->persist_dir) != 0)
            rc = -1;
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}

static cmq_stream_t *js_get_or_create(cmq_js_t *j, const char *name) {
    for (int i = 0; i < j->n; i++) {
        if (strcmp(j->slots[i].name, name) == 0)
            return j->slots[i].st;
    }
    if (j->n >= CMQ_JS_MAX) return NULL;
    cmq_stream_t *st = cmq_stream_create(name, 1024, 0);
    if (!st) return NULL;
    if (j->persist_dir[0] &&
        cmq_stream_set_cursor_path(st, j->persist_dir) != 0) {
        cmq_stream_destroy(st);
        return NULL;
    }
    snprintf(j->slots[j->n].name, sizeof(j->slots[j->n].name), "%s", name);
    j->slots[j->n].st = st;
    j->n++;
    return st;
}

int cmq_js_publish(cmq_js_t *j, const char *subject,
                   const uint8_t *val, size_t len) {
    if (!j) return -1;
    char name[CMQ_JS_NAME_MAX];
    int pr = cmq_js_parse(subject, name, sizeof(name));
    if (pr == -1) return 0;
    if (pr != 0) return -1;
    if (len == 0 || !val) return -1;
    cmq_mutex_lock(&j->lock);
    cmq_stream_t *st = js_get_or_create(j, name);
    int rc;
    if (!st)
        rc = -2;
    else if (cmq_stream_append(st, val, len) == 0)
        rc = -1;
    else
        rc = 1;
    cmq_mutex_unlock(&j->lock);
    return rc;
}

int cmq_js_last(cmq_js_t *j, const char *subject, uint8_t *out,
                size_t out_sz, size_t *out_len, uint64_t *out_seq) {
    if (!j || !out || !out_len || !out_seq) return -1;
    *out_len = 0;
    *out_seq = 0;
    char name[CMQ_JS_NAME_MAX];
    if (cmq_js_parse(subject, name, sizeof(name)) != 0)
        return -1;
    cmq_mutex_lock(&j->lock);
    cmq_stream_t *st = NULL;
    for (int i = 0; i < j->n; i++) {
        if (strcmp(j->slots[i].name, name) == 0) {
            st = j->slots[i].st;
            break;
        }
    }
    int rc = -1;
    if (st) {
        uint64_t seq = cmq_stream_last_seq(st);
        cmq_stream_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (seq > 0 && cmq_stream_read(st, seq, &msg) == 0) {
            if (msg.len <= out_sz) {
                if (msg.len)
                    memcpy(out, msg.data, msg.len);
                *out_len = msg.len;
                *out_seq = seq;
                rc = 0;
            }
            cmq_stream_msg_release(&msg);
        }
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}

int cmq_js_request(cmq_js_t *j, const char *subject, uint8_t *out,
                   size_t out_sz, size_t *out_len) {
    if (!out_len) return -1;
    *out_len = 0;
    if (!j || !subject || !out) return -1;
    int pr = cmq_js_parse(subject, NULL, 0);
    if (pr != 0) return -1;
    uint64_t seq = 0;
    if (cmq_js_last(j, subject, out, out_sz, out_len, &seq) == 0)
        return 1;
    *out_len = 0;
    return 0;
}
