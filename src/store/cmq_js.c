#define _POSIX_C_SOURCE 200809L
#include "cmq_js.h"
#include "cmq_stream.h"
#include "cmq_thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char name[CMQ_JS_NAME_MAX];
    cmq_stream_t *st;
    uint8_t *last;
    size_t last_len;
    uint64_t last_seq;
} cmq_js_slot_t;

struct cmq_js {
    cmq_js_slot_t slots[CMQ_JS_MAX];
    int n;
    char persist_dir[512];
    unsigned def_parts;
    uint64_t rotate_bytes;
    cmq_mutex_t lock;
};

static int js_replay_msgs(cmq_js_t *j, cmq_js_slot_t *s);
static int js_append_msg(cmq_js_t *j, const char *name, uint64_t seq,
                         const uint8_t *val, size_t len);
static int js_msgs_exists(cmq_js_t *j, const char *name);
static int js_load_parts(cmq_js_t *j, cmq_js_slot_t *s);
static int js_save_parts(cmq_js_t *j, const cmq_js_slot_t *s);
static int js_maybe_rotate_msgs(cmq_js_t *j, const char *name);
static int js_parts_exists(cmq_js_t *j, const char *name);
static unsigned js_parts_file_n(cmq_js_t *j, const char *name);
static uint64_t js_stream_append(cmq_stream_t *st, const uint8_t *val,
                                 size_t len);

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

int cmq_js_parse_cons(const char *subject, char *name, size_t ncap,
                      char *cons, size_t ccap) {
    if (!subject) return -1;
    if (subject[0] != '$') return -1;
    if (strncmp(subject, CMQ_JS_PREFIX, 4) != 0) return -1;
    const char *rest = subject + 4;
    if (!rest[0]) return -2;
    const char *dot = strchr(rest, '.');
    if (!dot || !dot[1] || strchr(dot + 1, '.')) return -2;
    size_t nlen = (size_t)(dot - rest);
    size_t clen = strlen(dot + 1);
    if (!name_ok(rest, nlen) || !name_ok(dot + 1, clen)) return -2;
    if (name && ncap) {
        if (nlen >= ncap) return -2;
        memcpy(name, rest, nlen);
        name[nlen] = '\0';
    }
    if (cons && ccap) {
        if (clen >= ccap) return -2;
        memcpy(cons, dot + 1, clen + 1);
    }
    return 0;
}

int cmq_js_parse_part(const char *subject, char *name, size_t ncap,
                      char *cons, size_t ccap, unsigned *part) {
    if (!subject) return -1;
    if (subject[0] != '$') return -1;
    if (strncmp(subject, CMQ_JS_PREFIX, 4) != 0) return -1;
    const char *rest = subject + 4;
    if (!rest[0]) return -2;
    const char *d1 = strchr(rest, '.');
    if (!d1) return -1;
    if (!d1[1]) return -2;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2 || !d2[1] || strchr(d2 + 1, '.')) return -2;
    size_t nlen = (size_t)(d1 - rest);
    size_t clen = (size_t)(d2 - (d1 + 1));
    const char *pt = d2 + 1;
    if (!name_ok(rest, nlen) || !name_ok(d1 + 1, clen)) return -2;
    if (!pt[0]) return -2;
    for (const char *q = pt; *q; q++) {
        if (*q < '0' || *q > '9') return -2;
    }
    unsigned long v = strtoul(pt, NULL, 10);
    if (v > 15) return -2;
    if (name && ncap) {
        if (nlen >= ncap) return -2;
        memcpy(name, rest, nlen);
        name[nlen] = '\0';
    }
    if (cons && ccap) {
        if (clen >= ccap) return -2;
        memcpy(cons, d1 + 1, clen);
        cons[clen] = '\0';
    }
    if (part) *part = (unsigned)v;
    return 0;
}

static void js_put_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t js_get_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static cmq_js_slot_t *js_find_slot(cmq_js_t *j, const char *name) {
    for (int i = 0; i < j->n; i++) {
        if (strcmp(j->slots[i].name, name) == 0)
            return &j->slots[i];
    }
    return NULL;
}

static cmq_stream_t *js_find(cmq_js_t *j, const char *name) {
    cmq_js_slot_t *s = js_find_slot(j, name);
    return s ? s->st : NULL;
}

static int js_last_path(const cmq_js_t *j, const char *name,
                        char *out, size_t cap) {
    if (!j || !j->persist_dir[0] || !name || !out || !cap) return -1;
    int w = snprintf(out, cap, "%s/%s.last", j->persist_dir, name);
    if (w < 0 || (size_t)w >= cap) return -1;
    return 0;
}

static int js_slot_set_last(cmq_js_slot_t *s, uint64_t seq,
                            const uint8_t *val, size_t len) {
    if (!s || !val || len == 0 || len > CMQ_JS_VAL_MAX) return -1;
    uint8_t *p = malloc(len);
    if (!p) return -1;
    memcpy(p, val, len);
    free(s->last);
    s->last = p;
    s->last_len = len;
    s->last_seq = seq;
    return 0;
}

static int js_save_last(cmq_js_t *j, const cmq_js_slot_t *s) {
    char path[640], tmp[648];
    if (js_last_path(j, s->name, path, sizeof(path)) != 0) return -1;
    int w = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (w < 0 || (size_t)w >= sizeof(tmp)) return -1;
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    uint8_t hdr[16];
    memcpy(hdr, "CMQL", 4);
    js_put_be64(hdr + 4, s->last_seq);
    hdr[12] = (uint8_t)((s->last_len >> 24) & 0xFF);
    hdr[13] = (uint8_t)((s->last_len >> 16) & 0xFF);
    hdr[14] = (uint8_t)((s->last_len >> 8) & 0xFF);
    hdr[15] = (uint8_t)(s->last_len & 0xFF);
    int ok = (fwrite(hdr, 1, 16, fp) == 16 &&
              fwrite(s->last, 1, s->last_len, fp) == s->last_len &&
              fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    fclose(fp);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int js_load_last(cmq_js_t *j, cmq_js_slot_t *s) {
    char path[640];
    if (js_last_path(j, s->name, path, sizeof(path)) != 0) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, fp) != 16 || memcmp(hdr, "CMQL", 4) != 0) {
        fclose(fp);
        return -1;
    }
    uint64_t seq = js_get_be64(hdr + 4);
    size_t len = ((size_t)hdr[12] << 24) | ((size_t)hdr[13] << 16) |
                 ((size_t)hdr[14] << 8) | (size_t)hdr[15];
    if (seq == 0 || len == 0 || len > CMQ_JS_VAL_MAX) {
        fclose(fp);
        return -1;
    }
    uint8_t *p = malloc(len);
    if (!p || fread(p, 1, len, fp) != len) {
        free(p);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    free(s->last);
    s->last = p;
    s->last_len = len;
    s->last_seq = seq;
    return 0;
}

cmq_js_t *cmq_js_create(void) {
    cmq_js_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    cmq_mutex_init(&j->lock);
    j->def_parts = 1;
    return j;
}

void cmq_js_destroy(cmq_js_t *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        cmq_stream_destroy(j->slots[i].st);
        free(j->slots[i].last);
    }
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
        if (j->slots[i].last_len == 0) {
            if (js_load_last(j, &j->slots[i]) != 0)
                rc = -1;
        } else if (js_save_last(j, &j->slots[i]) != 0) {
            rc = -1;
        }
        if (cmq_stream_partitions(j->slots[i].st) > 1 &&
            js_save_parts(j, &j->slots[i]) != 0)
            rc = -1;
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}

static cmq_js_slot_t *js_get_or_create_slot(cmq_js_t *j, const char *name) {
    cmq_js_slot_t *s = js_find_slot(j, name);
    if (s) return s;
    if (j->n >= CMQ_JS_MAX) return NULL;
    cmq_stream_t *st = cmq_stream_create(name, 1024, 0);
    if (!st) return NULL;
    if (j->persist_dir[0] &&
        cmq_stream_set_cursor_path(st, j->persist_dir) != 0) {
        cmq_stream_destroy(st);
        return NULL;
    }
    s = &j->slots[j->n];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->st = st;
    s->last = NULL;
    s->last_len = 0;
    s->last_seq = 0;
    int got_parts = 0;
    if (j->persist_dir[0])
        got_parts = js_load_parts(j, s);
    if (got_parts != 1 && j->def_parts > 1) {
        if (cmq_stream_set_partitions(st, j->def_parts) == 0 &&
            j->persist_dir[0])
            (void)js_save_parts(j, s);
    }
    if (j->persist_dir[0] && cmq_stream_last_seq(st) == 0)
        (void)js_replay_msgs(j, s);
    if (j->persist_dir[0])
        (void)js_load_last(j, s);
    j->n++;
    return s;
}

static int js_last_exists(cmq_js_t *j, const char *name) {
    char path[640];
    struct stat st;
    if (js_last_path(j, name, path, sizeof(path)) != 0) return 0;
    return stat(path, &st) == 0;
}

static int js_msgs_path(const cmq_js_t *j, const char *name,
                        char *out, size_t cap) {
    if (!j || !j->persist_dir[0] || !name || !out || !cap) return -1;
    int w = snprintf(out, cap, "%s/%s.msgs", j->persist_dir, name);
    if (w < 0 || (size_t)w >= cap) return -1;
    return 0;
}

static int js_msgs_exists(cmq_js_t *j, const char *name) {
    char path[640];
    struct stat st;
    if (js_msgs_path(j, name, path, sizeof(path)) != 0) return 0;
    return stat(path, &st) == 0;
}

static uint64_t js_stream_append(cmq_stream_t *st, const uint8_t *val,
                                 size_t len) {
    if (cmq_stream_partitions(st) > 1)
        return cmq_stream_append_key(st, val, len, val, len);
    return cmq_stream_append(st, val, len);
}

static int js_parts_path(const cmq_js_t *j, const char *name,
                         char *out, size_t cap) {
    if (!j || !j->persist_dir[0] || !name || !out || !cap) return -1;
    int w = snprintf(out, cap, "%s/%s.parts", j->persist_dir, name);
    if (w < 0 || (size_t)w >= cap) return -1;
    return 0;
}

static int js_parts_exists(cmq_js_t *j, const char *name) {
    char path[640];
    struct stat st;
    if (js_parts_path(j, name, path, sizeof(path)) != 0) return 0;
    return stat(path, &st) == 0;
}

static unsigned js_parts_file_n(cmq_js_t *j, const char *name) {
    char path[640];
    if (js_parts_path(j, name, path, sizeof(path)) != 0) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    unsigned n = 0;
    if (fscanf(fp, "CMQP\n%u\n", &n) != 1)
        n = 0;
    fclose(fp);
    if (n < 1 || n > CMQ_STREAM_MAX_PARTS) return 0;
    return n;
}

static int js_save_parts(cmq_js_t *j, const cmq_js_slot_t *s) {
    char path[640], tmp[648];
    if (js_parts_path(j, s->name, path, sizeof(path)) != 0) return -1;
    int w = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (w < 0 || (size_t)w >= sizeof(tmp)) return -1;
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    unsigned n = cmq_stream_partitions(s->st);
    int ok = (fprintf(fp, "CMQP\n%u\n", n) >= 0 &&
              fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    fclose(fp);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int js_load_parts(cmq_js_t *j, cmq_js_slot_t *s) {
    unsigned n = js_parts_file_n(j, s->name);
    if (n == 0) return 0;
    return cmq_stream_set_partitions(s->st, n) == 0 ? 1 : -1;
}

static int js_append_msg(cmq_js_t *j, const char *name, uint64_t seq,
                         const uint8_t *val, size_t len) {
    char path[640];
    if (js_msgs_path(j, name, path, sizeof(path)) != 0) return -1;
    if (!val || len == 0 || len > CMQ_JS_VAL_MAX || seq == 0) return -1;
    FILE *fp = fopen(path, "ab");
    if (!fp) return -1;
    uint8_t hdr[16];
    memcpy(hdr, "CMQM", 4);
    js_put_be64(hdr + 4, seq);
    hdr[12] = (uint8_t)((len >> 24) & 0xFF);
    hdr[13] = (uint8_t)((len >> 16) & 0xFF);
    hdr[14] = (uint8_t)((len >> 8) & 0xFF);
    hdr[15] = (uint8_t)(len & 0xFF);
    int ok = (fwrite(hdr, 1, 16, fp) == 16 &&
              fwrite(val, 1, len, fp) == len &&
              fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    fclose(fp);
    return ok ? 0 : -1;
}

static int js_replay_msgs(cmq_js_t *j, cmq_js_slot_t *s) {
    char path[640];
    if (js_msgs_path(j, s->name, path, sizeof(path)) != 0) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    for (;;) {
        uint8_t hdr[16];
        size_t nr = fread(hdr, 1, 16, fp);
        if (nr == 0) break;
        if (nr != 16 || memcmp(hdr, "CMQM", 4) != 0) {
            fclose(fp);
            return -1;
        }
        size_t len = ((size_t)hdr[12] << 24) | ((size_t)hdr[13] << 16) |
                     ((size_t)hdr[14] << 8) | (size_t)hdr[15];
        if (len == 0 || len > CMQ_JS_VAL_MAX) {
            fclose(fp);
            return -1;
        }
        uint8_t *p = malloc(len);
        if (!p || fread(p, 1, len, fp) != len) {
            free(p);
            fclose(fp);
            return -1;
        }
        uint64_t got = js_stream_append(s->st, p, len);
        free(p);
        if (got == 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

#define CMQ_JS_MSGS_KEEP 1024

static int js_copy_tail(const char *src, const char *dst, uint64_t start) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    if (start > 0 && fseek(in, (long)start, SEEK_SET) != 0) {
        fclose(in);
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    uint8_t buf[4096];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ok)
        ok = (fflush(out) == 0 && fsync(fileno(out)) == 0);
    fclose(out);
    fclose(in);
    if (!ok) {
        unlink(dst);
        return -1;
    }
    return 0;
}

static int js_compact_msgs(cmq_js_t *j, const char *name) {
    char path[640], tmp[648];
    if (js_msgs_path(j, name, path, sizeof(path)) != 0) return -1;
    uint64_t cap = j->rotate_bytes;
    if (cap == 0) return 0;
    int w = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (w < 0 || (size_t)w >= sizeof(tmp)) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    typedef struct { uint64_t off; uint32_t rec; } rec_i;
    rec_i hist[CMQ_JS_MSGS_KEEP];
    size_t count = 0;
    uint64_t off = 0;
    for (;;) {
        uint8_t hdr[16];
        size_t nr = fread(hdr, 1, 16, fp);
        if (nr == 0) break;
        if (nr != 16 || memcmp(hdr, "CMQM", 4) != 0) {
            fclose(fp);
            return -1;
        }
        size_t len = ((size_t)hdr[12] << 24) | ((size_t)hdr[13] << 16) |
                     ((size_t)hdr[14] << 8) | (size_t)hdr[15];
        if (len == 0 || len > CMQ_JS_VAL_MAX ||
            fseek(fp, (long)len, SEEK_CUR) != 0) {
            fclose(fp);
            return -1;
        }
        uint32_t rec = (uint32_t)(16 + len);
        hist[count % CMQ_JS_MSGS_KEEP].off = off;
        hist[count % CMQ_JS_MSGS_KEEP].rec = rec;
        count++;
        off += rec;
    }
    fclose(fp);
    if (count == 0) return 0;

    size_t keep_max = count < CMQ_JS_MSGS_KEEP ? count : CMQ_JS_MSGS_KEEP;
    size_t newest = (count - 1) % CMQ_JS_MSGS_KEEP;
    size_t take = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < keep_max; i++) {
        rec_i *r = &hist[(newest + CMQ_JS_MSGS_KEEP - i) % CMQ_JS_MSGS_KEEP];
        if (take > 0 && bytes + r->rec > cap) break;
        bytes += r->rec;
        take++;
    }
    rec_i *oldest = &hist[(newest + CMQ_JS_MSGS_KEEP - (take - 1)) %
                          CMQ_JS_MSGS_KEEP];
    uint64_t start = oldest->off;
    if (start == 0) return 0;
    if (js_copy_tail(path, tmp, start) != 0) return -1;
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int js_maybe_rotate_msgs(cmq_js_t *j, const char *name) {
    if (!j->rotate_bytes) return 0;
    char path[640];
    struct stat st;
    if (js_msgs_path(j, name, path, sizeof(path)) != 0) return 0;
    if (stat(path, &st) != 0) return 0;
    if ((uint64_t)st.st_size < j->rotate_bytes) return 0;
    return js_compact_msgs(j, name);
}

int cmq_js_set_msgs_rotate_bytes(cmq_js_t *j, uint64_t cap) {
    if (!j) return -1;
    cmq_mutex_lock(&j->lock);
    j->rotate_bytes = cap;
    cmq_mutex_unlock(&j->lock);
    return 0;
}

int cmq_js_publish(cmq_js_t *j, const char *subject,
                   const uint8_t *val, size_t len) {
    if (!j) return -1;
    char name[CMQ_JS_NAME_MAX];
    int pr = cmq_js_parse(subject, name, sizeof(name));
    if (pr == -1) return 0;
    if (pr == 0) {
        if (len == 0 || !val) return -1;
        cmq_mutex_lock(&j->lock);
        cmq_js_slot_t *s = js_get_or_create_slot(j, name);
        int rc;
        if (!s)
            rc = -2;
        else if (js_stream_append(s->st, val, len) == 0)
            rc = -1;
        else if (js_slot_set_last(s, cmq_stream_last_seq(s->st), val, len) != 0)
            rc = -1;
        else if (j->persist_dir[0] && js_save_last(j, s) != 0)
            rc = -1;
        else if (j->persist_dir[0] &&
                 js_append_msg(j, s->name, s->last_seq, val, len) != 0)
            rc = -1;
        else {
            if (j->persist_dir[0])
                (void)js_maybe_rotate_msgs(j, s->name);
            rc = 1;
        }
        cmq_mutex_unlock(&j->lock);
        return rc;
    }
    char cons[CMQ_JS_NAME_MAX];
    if (cmq_js_parse_cons(subject, name, sizeof(name), cons, sizeof(cons)) != 0)
        return -1;
    if (!val || len != 8) return -1;
    uint64_t seq = js_get_be64(val);
    if (seq == 0) return -1;
    cmq_mutex_lock(&j->lock);
    cmq_stream_t *st = js_find(j, name);
    if (!st && j->persist_dir[0] &&
        (js_msgs_exists(j, name) || js_parts_exists(j, name))) {
        cmq_js_slot_t *s = js_get_or_create_slot(j, name);
        st = s ? s->st : NULL;
    }
    int rc = -1;
    if (st && cmq_stream_consumer_ack(st, cons, seq) == 0)
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
    cmq_js_slot_t *s = js_find_slot(j, name);
    if (!s && j->persist_dir[0] && js_last_exists(j, name))
        s = js_get_or_create_slot(j, name);
    int rc = -1;
    if (s && s->last_len) {
        if (s->last_len <= out_sz) {
            memcpy(out, s->last, s->last_len);
            *out_len = s->last_len;
            *out_seq = s->last_seq;
            rc = 0;
        }
    } else if (s && s->st) {
        uint64_t seq = cmq_stream_last_seq(s->st);
        cmq_stream_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (seq > 0 && cmq_stream_read(s->st, seq, &msg) == 0) {
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

int cmq_js_consume(cmq_js_t *j, const char *subject, uint8_t *out,
                   size_t out_sz, size_t *out_len) {
    if (!out_len) return -1;
    *out_len = 0;
    if (!j || !subject || !out) return -1;
    char name[CMQ_JS_NAME_MAX], cons[CMQ_JS_NAME_MAX];
    unsigned part = 0;
    int pr = cmq_js_parse_cons(subject, name, sizeof(name), cons, sizeof(cons));
    if (pr != 0) {
        if (cmq_js_parse_part(subject, name, sizeof(name), cons, sizeof(cons),
                              &part) != 0)
            return -1;
        return cmq_js_consume_part(j, subject, part, out, out_sz, out_len);
    }
    cmq_mutex_lock(&j->lock);
    cmq_stream_t *st = js_find(j, name);
    if (!st && j->persist_dir[0] &&
        (js_msgs_exists(j, name) || js_parts_exists(j, name))) {
        cmq_js_slot_t *s = js_get_or_create_slot(j, name);
        st = s ? s->st : NULL;
    }
    int rc = 0;
    if (st && cmq_stream_add_consumer(st, cons) == 0) {
        uint64_t seq = cmq_stream_consumer_next(st, cons);
        cmq_stream_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (seq > 0 && cmq_stream_read(st, seq, &msg) == 0) {
            if (8 + msg.len <= out_sz) {
                js_put_be64(out, seq);
                if (msg.len)
                    memcpy(out + 8, msg.data, msg.len);
                *out_len = 8 + msg.len;
                rc = 1;
            }
            cmq_stream_msg_release(&msg);
        }
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}

int cmq_js_set_default_partitions(cmq_js_t *j, unsigned n) {
    if (!j || n < 1 || n > CMQ_STREAM_MAX_PARTS) return -1;
    cmq_mutex_lock(&j->lock);
    j->def_parts = n;
    cmq_mutex_unlock(&j->lock);
    return 0;
}

int cmq_js_set_partitions(cmq_js_t *j, const char *name, unsigned n) {
    if (!j || !name || !name_ok(name, strlen(name))) return -1;
    if (n < 1 || n > CMQ_STREAM_MAX_PARTS) return -1;
    cmq_mutex_lock(&j->lock);
    cmq_js_slot_t *s = js_get_or_create_slot(j, name);
    int rc = -1;
    if (s && cmq_stream_set_partitions(s->st, n) == 0) {
        if (j->persist_dir[0] && js_save_parts(j, s) != 0)
            rc = -1;
        else
            rc = 0;
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}

unsigned cmq_js_partitions(cmq_js_t *j, const char *name) {
    if (!j || !name || !name_ok(name, strlen(name))) return 0;
    cmq_mutex_lock(&j->lock);
    unsigned n = 1;
    cmq_js_slot_t *s = js_find_slot(j, name);
    if (s)
        n = cmq_stream_partitions(s->st);
    else if (j->persist_dir[0]) {
        unsigned f = js_parts_file_n(j, name);
        if (f) n = f;
    }
    cmq_mutex_unlock(&j->lock);
    return n;
}

int cmq_js_consume_part(cmq_js_t *j, const char *subject, unsigned part,
                        uint8_t *out, size_t out_sz, size_t *out_len) {
    if (!out_len) return -1;
    *out_len = 0;
    if (!j || !subject || !out) return -1;
    char name[CMQ_JS_NAME_MAX], cons[CMQ_JS_NAME_MAX];
    unsigned use = part;
    if (cmq_js_parse_part(subject, name, sizeof(name), cons, sizeof(cons),
                          &use) != 0 &&
        cmq_js_parse_cons(subject, name, sizeof(name), cons, sizeof(cons)) != 0)
        return -1;
    cmq_mutex_lock(&j->lock);
    cmq_stream_t *st = js_find(j, name);
    if (!st && j->persist_dir[0] &&
        (js_msgs_exists(j, name) || js_parts_exists(j, name))) {
        cmq_js_slot_t *s = js_get_or_create_slot(j, name);
        st = s ? s->st : NULL;
    }
    int rc = 0;
    if (st && cmq_stream_add_consumer(st, cons) == 0) {
        uint64_t seq = cmq_stream_consumer_next_part(st, cons, use);
        cmq_stream_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (seq > 0 && cmq_stream_read(st, seq, &msg) == 0) {
            if (8 + msg.len <= out_sz) {
                js_put_be64(out, seq);
                if (msg.len)
                    memcpy(out + 8, msg.data, msg.len);
                *out_len = 8 + msg.len;
                rc = 1;
            }
            cmq_stream_msg_release(&msg);
        }
    }
    cmq_mutex_unlock(&j->lock);
    return rc;
}
