#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "cmq_slab.h"
#include "cmq_platform.h"

/* A slab page with capacity objects of size obj_size */
typedef struct cmq_slab_page {
    uint8_t *raw;      /* original malloc() pointer for freeing */
    uint8_t *mem;      /* aligned start of object storage */
    size_t capacity;   /* number of objects this page can hold */
    size_t obj_size;   /* size of each object */
    size_t used;       /* high-water bump allocations on this page */
    int use_ptr_next;  /* 1 = inline next pointer stored in memory, 0 = index-based freelist */
    union {
        void *head;           /* freelist head when use_ptr_next == 1 */
        int head_index;         /* freelist head index when use_ptr_next == 0 */
    } freelist;
    struct cmq_slab_page *next;
} cmq_slab_page_t;

struct cmq_slab {
    size_t obj_size;
    size_t capacity;      /* initial capacity per page */
    cmq_slab_page_t *head;
    size_t total_allocated; /* total number of objects ever allocated (per tests) */
};

static inline uint8_t *slab_mem_align(uint8_t *mem) {
    uintptr_t a = (uintptr_t)mem;
    size_t off = (16 - (a % 16)) % 16;
    return mem + off;
}

static int size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static cmq_slab_page_t *slab_page_create(size_t obj_size, size_t capacity) {
    if (obj_size == 0) return NULL;
    cmq_slab_page_t *p = (cmq_slab_page_t *)malloc(sizeof(*p));
    if (!p) return NULL;
    p->obj_size = obj_size;
    p->capacity = capacity ? capacity : 4;
    size_t bytes;
    if (size_mul(p->capacity, p->obj_size, &bytes) != 0 ||
        bytes > SIZE_MAX - 16) {
        free(p);
        return NULL;
    }
    p->raw = (uint8_t *)malloc(bytes + 16);
    if (!p->raw) {
        free(p);
        return NULL;
    }
    p->mem = slab_mem_align(p->raw);
    p->used = 0;
    p->next = NULL;
    p->use_ptr_next = (obj_size >= sizeof(void *));
    if (p->use_ptr_next) {
        p->freelist.head = NULL;
    } else {
        p->freelist.head_index = -1;
    }
    return p;
}

cmq_slab_t *cmq_slab_create(size_t obj_size, size_t capacity) {
    /* Index freelist stores uint32_t inside free objects. */
    if (obj_size < sizeof(uint32_t)) return NULL;
    /* head_index is int — index freelist cannot exceed INT_MAX slots. */
    if (obj_size < sizeof(void *) && capacity > (size_t)INT_MAX)
        return NULL;
    cmq_slab_t *slab = (cmq_slab_t *)malloc(sizeof(*slab));
    if (!slab) return NULL;
    slab->obj_size = obj_size;
    slab->capacity = (capacity > 0) ? capacity : 4;
    slab->head = slab_page_create(obj_size, slab->capacity);
    if (!slab->head) {
        free(slab);
        return NULL;
    }
    slab->total_allocated = 0;
    return slab;
}

void cmq_slab_destroy(cmq_slab_t *slab) {
    if (!slab) return;
    cmq_slab_page_t *cur = slab->head;
    while (cur) {
        cmq_slab_page_t *n = cur->next;
        free(cur->raw);
        free(cur);
        cur = n;
    }
    free(slab);
}

static cmq_slab_page_t *slab_find_page(cmq_slab_t *slab, void *obj) {
    uint8_t *p = (uint8_t *)obj;
    cmq_slab_page_t *cur = slab->head;
    while (cur) {
        size_t span;
        if (size_mul(cur->capacity, cur->obj_size, &span) != 0) {
            cur = cur->next;
            continue;
        }
        if (p >= cur->mem && p < cur->mem + span)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int slab_on_freelist(cmq_slab_page_t *page, void *obj, size_t idx) {
    size_t guard = page->capacity + 1;
    if (page->use_ptr_next) {
        void *x = page->freelist.head;
        while (x && guard-- > 0) {
            if (x == obj) return 1;
            x = *((void **)x);
        }
    } else {
        int i = page->freelist.head_index;
        while (i >= 0 && guard-- > 0) {
            if ((size_t)i == idx) return 1;
            void *o = page->mem + (size_t)i * page->obj_size;
            i = (int)*((uint32_t *)o);
        }
    }
    return 0;
}

void *cmq_slab_alloc(cmq_slab_t *slab) {
    if (!slab || !slab->head) return NULL;
    cmq_slab_page_t *cur = slab->head;
    while (cur) {
        if (cur->use_ptr_next) {
            if (cur->freelist.head != NULL) {
                void *obj = cur->freelist.head;
                cur->freelist.head = *((void **)obj);
                slab->total_allocated++;
                return obj;
            }
            if (cur->used < cur->capacity) {
                void *obj = cur->mem + (cur->used * cur->obj_size);
                cur->used++;
                slab->total_allocated++;
                return obj;
            }
        } else {
            if (cur->freelist.head_index != -1) {
                int idx = cur->freelist.head_index;
                void *obj = cur->mem + ((size_t)idx * cur->obj_size);
                cur->freelist.head_index = (int)*((uint32_t *)obj);
                slab->total_allocated++;
                return obj;
            }
            if (cur->used < cur->capacity) {
                void *obj = cur->mem + (cur->used * cur->obj_size);
                cur->used++;
                slab->total_allocated++;
                return obj;
            }
        }
        cur = cur->next;
    }
    if (slab->capacity > SIZE_MAX / 2) return NULL;
    size_t new_cap = slab->capacity ? slab->capacity * 2 : 4;
    if (slab->obj_size < sizeof(void *) && new_cap > (size_t)INT_MAX)
        return NULL;
    cmq_slab_page_t *np = slab_page_create(slab->obj_size, new_cap);
    if (!np) return NULL;
    cmq_slab_page_t *last = slab->head;
    while (last->next) last = last->next;
    last->next = np;
    if (np->used < np->capacity) {
        void *obj = np->mem + (np->used * np->obj_size);
        np->used++;
        slab->total_allocated++;
        return obj;
    }
    return NULL;
}

void cmq_slab_free(cmq_slab_t *slab, void *obj) {
    if (!slab || !obj) return;
    cmq_slab_page_t *page = slab_find_page(slab, obj);
    if (!page) return;
    uintptr_t off = (uintptr_t)((uint8_t *)obj - page->mem);
    if (off % page->obj_size != 0) return;
    size_t idx = off / page->obj_size;
    if (idx >= page->used) return;
    if (slab_on_freelist(page, obj, idx)) return; /* double-free */

    if (page->use_ptr_next) {
        void *head = page->freelist.head;
        page->freelist.head = obj;
        *((void **)obj) = head;
    } else {
        uint32_t next = (uint32_t)page->freelist.head_index;
        *((uint32_t *)obj) = next;
        page->freelist.head_index = (int)idx;
    }
    if (slab->total_allocated > 0)
        slab->total_allocated--;
}

size_t cmq_slab_count(cmq_slab_t *slab) {
    if (!slab) return 0;
    return slab->total_allocated;
}
