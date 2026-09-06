#define _POSIX_C_SOURCE 200809L
#include "cmq_kvb.h"
#include "cmq_kv.h"
#include "cmq_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[CMQ_KVB_BUCKET_MAX];
    cmq_kv_t *kv;
} cmq_kvb_slot_t;

struct cmq_kvb {
    cmq_kvb_slot_t slots[CMQ_KVB_MAX];
    int n;
    char persist_dir[256];
    cmq_mutex_t lock;
};

static int bucket_ok(const char *s, size_t n) {
    if (n == 0 || n >= CMQ_KVB_BUCKET_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

int cmq_kvb_parse(const char *subject, char *bucket, size_t bcap,
                  char *key, size_t kcap) {
    if (!subject) return -1;
    if (subject[0] != '$') return -1;
    if (strncmp(subject, CMQ_KVB_PREFIX, 4) != 0) return -1;
    const char *rest = subject + 4;
    if (!rest[0]) return -2;
    const char *dot = strchr(rest, '.');
    if (!dot || !dot[1]) return -2;
    size_t blen = (size_t)(dot - rest);
    if (!bucket_ok(rest, blen)) return -2;
    size_t klen = strlen(dot + 1);
    if (klen == 0 || klen >= CMQ_KV_KEY_MAX) return -2;
    if (bucket && bcap) {
        if (blen >= bcap) return -2;
        memcpy(bucket, rest, blen);
        bucket[blen] = '\0';
    }
    if (key && kcap) {
        if (klen >= kcap) return -2;
        memcpy(key, dot + 1, klen + 1);
    }
    return 0;
}

cmq_kvb_t *cmq_kvb_create(void) {
    cmq_kvb_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    cmq_mutex_init(&b->lock);
    return b;
}

void cmq_kvb_destroy(cmq_kvb_t *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++)
        cmq_kv_destroy(b->slots[i].kv);
    cmq_mutex_destroy(&b->lock);
    free(b);
}

int cmq_kvb_set_persist(cmq_kvb_t *b, const char *dir) {
    if (!b || !dir || !dir[0]) return -1;
    size_t n = strnlen(dir, sizeof(b->persist_dir));
    if (n == 0 || n >= sizeof(b->persist_dir)) return -1;
    cmq_mutex_lock(&b->lock);
    memcpy(b->persist_dir, dir, n + 1);
    int rc = 0;
    for (int i = 0; i < b->n; i++) {
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "kv_%s", b->slots[i].name);
        if (cmq_kv_set_persist(b->slots[i].kv, b->persist_dir, prefix) != 0)
            rc = -1;
    }
    cmq_mutex_unlock(&b->lock);
    return rc;
}

static cmq_kv_t *kvb_get_or_create(cmq_kvb_t *b, const char *bucket) {
    for (int i = 0; i < b->n; i++) {
        if (strcmp(b->slots[i].name, bucket) == 0)
            return b->slots[i].kv;
    }
    if (b->n >= CMQ_KVB_MAX) return NULL;
    cmq_kv_t *kv = cmq_kv_create(0);
    if (!kv) return NULL;
    if (b->persist_dir[0]) {
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "kv_%s", bucket);
        if (cmq_kv_set_persist(kv, b->persist_dir, prefix) != 0) {
            cmq_kv_destroy(kv);
            return NULL;
        }
    }
    snprintf(b->slots[b->n].name, sizeof(b->slots[b->n].name), "%s", bucket);
    b->slots[b->n].kv = kv;
    b->n++;
    return kv;
}

int cmq_kvb_publish(cmq_kvb_t *b, const char *subject,
                    const uint8_t *val, size_t len) {
    if (!b) return -1;
    char bucket[CMQ_KVB_BUCKET_MAX], key[CMQ_KV_KEY_MAX];
    int pr = cmq_kvb_parse(subject, bucket, sizeof(bucket), key, sizeof(key));
    if (pr == -1) return 0;
    if (pr != 0) return -1;
    cmq_mutex_lock(&b->lock);
    cmq_kv_t *kv = kvb_get_or_create(b, bucket);
    int rc;
    if (!kv)
        rc = -2;
    else if (len == 0) {
        (void)cmq_kv_del(kv, key);
        rc = 1;
    }
    else {
        int p = cmq_kv_put(kv, key, val, len);
        if (p == 0) rc = 1;
        else if (p == -2) rc = -2;
        else if (p == -3) rc = -3;
        else rc = -1;
    }
    cmq_mutex_unlock(&b->lock);
    return rc;
}

int cmq_kvb_get(cmq_kvb_t *b, const char *subject, uint8_t *out,
                size_t out_sz, size_t *out_len) {
    if (!b || !out || !out_len) return -1;
    char bucket[CMQ_KVB_BUCKET_MAX], key[CMQ_KV_KEY_MAX];
    if (cmq_kvb_parse(subject, bucket, sizeof(bucket), key, sizeof(key)) != 0)
        return -1;
    cmq_mutex_lock(&b->lock);
    cmq_kv_t *kv = NULL;
    for (int i = 0; i < b->n; i++) {
        if (strcmp(b->slots[i].name, bucket) == 0) {
            kv = b->slots[i].kv;
            break;
        }
    }
    int rc = kv ? cmq_kv_get(kv, key, out, out_sz, out_len) : -1;
    cmq_mutex_unlock(&b->lock);
    return rc;
}
