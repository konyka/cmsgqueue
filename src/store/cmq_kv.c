#define _POSIX_C_SOURCE 200809L
#include "cmq_kv.h"
#include "cmq_filestore.h"
#include "cmq_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t used;
    uint16_t key_len;
    uint16_t val_len;
    char key[CMQ_KV_KEY_MAX];
    uint8_t val[CMQ_KV_VAL_MAX];
} cmq_kv_slot_t;

struct cmq_kv {
    cmq_kv_slot_t *slots;
    size_t cap;
    size_t count;
    cmq_filestore_t *fs;
    cmq_mutex_t lock;
};

static int kv_key_safe(const char *s) {
    if (!s || !*s) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, n++) {
        unsigned char c = *p;
        if (n >= CMQ_KV_KEY_MAX) return 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static int kv_find(cmq_kv_t *kv, const char *key) {
    for (size_t i = 0; i < kv->cap; i++) {
        if (kv->slots[i].used && strcmp(kv->slots[i].key, key) == 0)
            return (int)i;
    }
    return -1;
}

static int kv_free_slot(cmq_kv_t *kv) {
    for (size_t i = 0; i < kv->cap; i++) {
        if (!kv->slots[i].used)
            return (int)i;
    }
    return -1;
}

static void kv_apply(cmq_kv_t *kv, const char *key, const uint8_t *val,
                     size_t len) {
    int idx = kv_find(kv, key);
    if (len == 0) {
        if (idx >= 0) {
            memset(&kv->slots[idx], 0, sizeof(kv->slots[idx]));
            if (kv->count > 0) kv->count--;
        }
        return;
    }
    if (idx < 0) {
        idx = kv_free_slot(kv);
        if (idx < 0) return;
        kv->count++;
    }
    size_t klen = strnlen(key, CMQ_KV_KEY_MAX);
    kv->slots[idx].used = 1;
    kv->slots[idx].key_len = (uint16_t)klen;
    kv->slots[idx].val_len = (uint16_t)len;
    memcpy(kv->slots[idx].key, key, klen);
    kv->slots[idx].key[klen] = '\0';
    memcpy(kv->slots[idx].val, val, len);
}

static int kv_persist(cmq_kv_t *kv, const char *key, const uint8_t *val,
                      size_t len) {
    if (!kv->fs) return 0;
    uint8_t buf[6 + CMQ_KV_KEY_MAX + CMQ_KV_VAL_MAX];
    size_t n = 0;
    size_t klen = strnlen(key, CMQ_KV_KEY_MAX);
    if (cmq_filestore_key_encode(buf, sizeof(buf), key, klen, val, len, &n) != 0)
        return -1;
    uint64_t seq = 0;
    return cmq_filestore_append(kv->fs, buf, n, &seq);
}

static int kv_replay(cmq_kv_t *kv) {
    if (!kv->fs) return 0;
    uint64_t last = cmq_filestore_last_seq(kv->fs);
    for (uint64_t seq = 1; seq <= last; seq++) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (cmq_filestore_read(kv->fs, seq, &data, &len) != 0)
            continue;
        const uint8_t *k = NULL, *v = NULL;
        size_t klen = 0, vlen = 0;
        if (cmq_filestore_key_decode(data, len, &k, &klen, &v, &vlen) == 0 &&
            klen > 0 && klen < CMQ_KV_KEY_MAX) {
            char key[CMQ_KV_KEY_MAX];
            memcpy(key, k, klen);
            key[klen] = '\0';
            if (kv_key_safe(key) && vlen <= CMQ_KV_VAL_MAX)
                kv_apply(kv, key, v, vlen);
        }
        free(data);
    }
    return 0;
}

cmq_kv_t *cmq_kv_create(size_t slots) {
    if (slots == 0) slots = CMQ_KV_SLOTS_MAX;
    if (slots > CMQ_KV_SLOTS_MAX) return NULL;
    cmq_kv_t *kv = calloc(1, sizeof(*kv));
    if (!kv) return NULL;
    kv->slots = calloc(slots, sizeof(*kv->slots));
    if (!kv->slots) {
        free(kv);
        return NULL;
    }
    kv->cap = slots;
    cmq_mutex_init(&kv->lock);
    return kv;
}

void cmq_kv_destroy(cmq_kv_t *kv) {
    if (!kv) return;
    if (kv->fs)
        cmq_filestore_destroy(kv->fs);
    cmq_mutex_destroy(&kv->lock);
    free(kv->slots);
    free(kv);
}

int cmq_kv_set_persist(cmq_kv_t *kv, const char *dir, const char *prefix) {
    if (!kv || !dir || !prefix || !prefix[0]) return -1;
    cmq_mutex_lock(&kv->lock);
    int rc = -1;
    if (!kv->fs) {
        kv->fs = cmq_filestore_create(dir, prefix);
        if (kv->fs)
            rc = kv_replay(kv);
    }
    cmq_mutex_unlock(&kv->lock);
    return rc;
}

int cmq_kv_put(cmq_kv_t *kv, const char *key, const uint8_t *val, size_t len) {
    if (!kv || !kv_key_safe(key) || (len > 0 && !val)) return -1;
    if (len > CMQ_KV_VAL_MAX) return -3;
    cmq_mutex_lock(&kv->lock);
    int idx = kv_find(kv, key);
    if (idx < 0 && kv_free_slot(kv) < 0) {
        cmq_mutex_unlock(&kv->lock);
        return -2;
    }
    if (kv_persist(kv, key, val, len) != 0) {
        cmq_mutex_unlock(&kv->lock);
        return -1;
    }
    kv_apply(kv, key, val, len);
    cmq_mutex_unlock(&kv->lock);
    return 0;
}

int cmq_kv_get(cmq_kv_t *kv, const char *key, uint8_t *out, size_t out_sz,
               size_t *out_len) {
    if (!kv || !kv_key_safe(key) || !out || !out_len) return -1;
    cmq_mutex_lock(&kv->lock);
    int idx = kv_find(kv, key);
    int rc = -1;
    if (idx >= 0) {
        if (out_sz < kv->slots[idx].val_len)
            rc = -2;
        else {
            memcpy(out, kv->slots[idx].val, kv->slots[idx].val_len);
            *out_len = kv->slots[idx].val_len;
            rc = 0;
        }
    }
    cmq_mutex_unlock(&kv->lock);
    return rc;
}

int cmq_kv_del(cmq_kv_t *kv, const char *key) {
    if (!kv || !kv_key_safe(key)) return -1;
    cmq_mutex_lock(&kv->lock);
    int idx = kv_find(kv, key);
    int rc = -1;
    if (idx >= 0) {
        if (kv_persist(kv, key, NULL, 0) != 0)
            rc = -1;
        else {
            kv_apply(kv, key, NULL, 0);
            rc = 0;
        }
    }
    cmq_mutex_unlock(&kv->lock);
    return rc;
}

size_t cmq_kv_count(cmq_kv_t *kv) {
    if (!kv) return 0;
    cmq_mutex_lock(&kv->lock);
    size_t n = kv->count;
    cmq_mutex_unlock(&kv->lock);
    return n;
}
