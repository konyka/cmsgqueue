#define _POSIX_C_SOURCE 200809L
#include "cmq_txn.h"
#include "cmq_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char subject[CMQ_TXN_SUB_MAX];
    uint16_t len;
    uint8_t data[CMQ_TXN_PAY_MAX];
} cmq_txn_op_t;

typedef struct {
    uint64_t id;
    uint8_t used;
    uint8_t nops;
    cmq_txn_op_t ops[CMQ_TXN_OPS];
} cmq_txn_slot_t;

struct cmq_txn {
    cmq_txn_slot_t slots[CMQ_TXN_MAX];
    uint64_t done[256];
    size_t ndone;
    char log_path[600];
    cmq_mutex_t lock;
};

static void put_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static int txn_token_safe(const char *s) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static int txn_dir_safe(const char *dir) {
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

static int txn_find(cmq_txn_t *t, uint64_t id) {
    for (int i = 0; i < CMQ_TXN_MAX; i++) {
        if (t->slots[i].used && t->slots[i].id == id)
            return i;
    }
    return -1;
}

static int txn_free_slot(cmq_txn_t *t) {
    for (int i = 0; i < CMQ_TXN_MAX; i++) {
        if (!t->slots[i].used)
            return i;
    }
    return -1;
}

static int txn_done_has(cmq_txn_t *t, uint64_t id) {
    for (size_t i = 0; i < t->ndone; i++) {
        if (t->done[i] == id)
            return 1;
    }
    return 0;
}

static void txn_done_add(cmq_txn_t *t, uint64_t id) {
    if (txn_done_has(t, id)) return;
    if (t->ndone < 256)
        t->done[t->ndone++] = id;
    else
        t->done[t->ndone % 256] = id;
}

static int txn_log_commit(cmq_txn_t *t, uint64_t id) {
    if (!t->log_path[0]) return 0;
    FILE *fp = fopen(t->log_path, "ab");
    if (!fp) return -1;
    uint8_t rec[9];
    rec[0] = 'C';
    put_be64(rec + 1, id);
    int ok = (fwrite(rec, 1, sizeof(rec), fp) == sizeof(rec) &&
              fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    fclose(fp);
    return ok ? 0 : -1;
}

static int txn_log_load(cmq_txn_t *t) {
    if (!t->log_path[0]) return 0;
    FILE *fp = fopen(t->log_path, "rb");
    if (!fp) return 0;
    uint8_t rec[9];
    while (fread(rec, 1, sizeof(rec), fp) == sizeof(rec)) {
        if (rec[0] == 'C')
            txn_done_add(t, get_be64(rec + 1));
    }
    fclose(fp);
    return 0;
}

cmq_txn_t *cmq_txn_create(void) {
    cmq_txn_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    cmq_mutex_init(&t->lock);
    return t;
}

void cmq_txn_destroy(cmq_txn_t *t) {
    if (!t) return;
    cmq_mutex_destroy(&t->lock);
    free(t);
}

int cmq_txn_set_log(cmq_txn_t *t, const char *dir) {
    if (!t || !txn_dir_safe(dir)) return -1;
    cmq_mutex_lock(&t->lock);
    int rc = -1;
    if (snprintf(t->log_path, sizeof(t->log_path), "%s/cmq.txn", dir) <
        (int)sizeof(t->log_path))
        rc = txn_log_load(t);
    else
        t->log_path[0] = '\0';
    cmq_mutex_unlock(&t->lock);
    return rc;
}

int cmq_txn_encode(uint8_t *out, size_t cap, uint64_t id, uint8_t op,
                   size_t *out_len) {
    if (!out || cap < CMQ_TXN_HDR_LEN || !out_len || id == 0)
        return -1;
    if (op < CMQ_TXN_BEGIN || op > CMQ_TXN_ABORT) return -1;
    memcpy(out, CMQ_TXN_MAGIC, 4);
    put_be64(out + 4, id);
    out[12] = op;
    *out_len = CMQ_TXN_HDR_LEN;
    return 0;
}

int cmq_txn_parse(const uint8_t *hdr, size_t n, uint64_t *id, uint8_t *op) {
    if (!hdr || n < CMQ_TXN_HDR_LEN || !id || !op) return -1;
    if (memcmp(hdr, CMQ_TXN_MAGIC, 4) != 0) return -1;
    uint64_t tid = get_be64(hdr + 4);
    uint8_t o = hdr[12];
    if (tid == 0 || o < CMQ_TXN_BEGIN || o > CMQ_TXN_ABORT) return -1;
    *id = tid;
    *op = o;
    return 0;
}

int cmq_txn_begin(cmq_txn_t *t, uint64_t id) {
    if (!t || id == 0) return -1;
    cmq_mutex_lock(&t->lock);
    int rc = 0;
    if (txn_done_has(t, id)) {
        rc = -1;
    } else if (txn_find(t, id) >= 0) {
        rc = 0;
    } else {
        int idx = txn_free_slot(t);
        if (idx < 0)
            rc = -2;
        else {
            memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
            t->slots[idx].used = 1;
            t->slots[idx].id = id;
        }
    }
    cmq_mutex_unlock(&t->lock);
    return rc;
}

int cmq_txn_add(cmq_txn_t *t, uint64_t id, const char *subject,
                const uint8_t *data, size_t len) {
    if (!t || id == 0 || !txn_token_safe(subject) || (len > 0 && !data))
        return -1;
    if (len > CMQ_TXN_PAY_MAX) return -3;
    size_t slen = strnlen(subject, CMQ_TXN_SUB_MAX);
    if (slen == 0 || slen >= CMQ_TXN_SUB_MAX) return -1;
    cmq_mutex_lock(&t->lock);
    int idx = txn_find(t, id);
    int rc = -1;
    if (idx >= 0 && t->slots[idx].nops < CMQ_TXN_OPS) {
        cmq_txn_op_t *op = &t->slots[idx].ops[t->slots[idx].nops++];
        memcpy(op->subject, subject, slen);
        op->subject[slen] = '\0';
        op->len = (uint16_t)len;
        if (len > 0)
            memcpy(op->data, data, len);
        rc = 0;
    }
    cmq_mutex_unlock(&t->lock);
    return rc;
}

int cmq_txn_commit(cmq_txn_t *t, uint64_t id, cmq_txn_apply_fn fn, void *ctx) {
    if (!t || id == 0) return -1;
    cmq_txn_op_t copy[CMQ_TXN_OPS];
    int nops = 0;
    cmq_mutex_lock(&t->lock);
    if (txn_done_has(t, id)) {
        cmq_mutex_unlock(&t->lock);
        return 0;
    }
    int idx = txn_find(t, id);
    if (idx < 0) {
        cmq_mutex_unlock(&t->lock);
        return -1;
    }
    nops = t->slots[idx].nops;
    if (nops > 0)
        memcpy(copy, t->slots[idx].ops, (size_t)nops * sizeof(copy[0]));
    if (txn_log_commit(t, id) != 0) {
        cmq_mutex_unlock(&t->lock);
        return -1;
    }
    txn_done_add(t, id);
    memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
    cmq_mutex_unlock(&t->lock);
    for (int i = 0; i < nops && fn; i++) {
        if (fn(ctx, copy[i].subject, copy[i].data, copy[i].len) != 0)
            return -1;
    }
    return 0;
}

int cmq_txn_abort(cmq_txn_t *t, uint64_t id) {
    if (!t || id == 0) return -1;
    cmq_mutex_lock(&t->lock);
    int idx = txn_find(t, id);
    int rc = -1;
    if (idx >= 0) {
        memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
        rc = 0;
    }
    cmq_mutex_unlock(&t->lock);
    return rc;
}

int cmq_txn_was_committed(cmq_txn_t *t, uint64_t id) {
    if (!t || id == 0) return 0;
    cmq_mutex_lock(&t->lock);
    int rc = txn_done_has(t, id);
    cmq_mutex_unlock(&t->lock);
    return rc;
}
