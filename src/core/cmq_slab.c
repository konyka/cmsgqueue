#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cmq_slab.h"
#include "cmq_platform.h"

/* A slab page with capacity objects of size obj_size */
typedef struct cmq_slab_page {
    uint8_t *raw;      /* original malloc() pointer for freeing */
    uint8_t *mem;      /* aligned start of object storage */
    size_t capacity;   /* number of objects this page can hold */
    size_t obj_size;   /* size of each object */
    size_t used;       /* number of allocations performed on this page (including freed if using) */
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

/* helpers */
static inline uint8_t *slab_mem_align(uint8_t *mem) {
    uintptr_t a = (uintptr_t)mem;
    size_t off = (16 - (a % 16)) % 16;
    return mem + off;
}

static cmq_slab_page_t *slab_page_create(size_t obj_size, size_t capacity) {
    cmq_slab_page_t *p = (cmq_slab_page_t *)malloc(sizeof(*p));
    if (!p) return NULL;
    p->obj_size = obj_size;
    p->capacity = capacity ? capacity : 4;
    size_t bytes = p->capacity * p->obj_size;
    /* allocate a little extra to allow for 16-byte alignment */
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
    if (obj_size == 0) return NULL;
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
        if (p >= cur->mem && p < cur->mem + cur->capacity * cur->obj_size) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

void *cmq_slab_alloc(cmq_slab_t *slab) {
    if (!slab || !slab->head) return NULL;
    cmq_slab_page_t *cur = slab->head;
    /* try to satisfy from existing pages, prefer freelist */
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
                void *obj = cur->mem + (idx * cur->obj_size);
                cur->freelist.head_index = *((uint32_t *)obj);
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
    /* need a new page */
    size_t new_cap = slab->capacity ? slab->capacity * 2 : 4;
    cmq_slab_page_t *np = slab_page_create(slab->obj_size, new_cap);
    if (!np) return NULL;
    /* append to list */
    cmq_slab_page_t *last = slab->head;
    while (last->next) last = last->next;
    last->next = np;
    /* allocate from new page */
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
    if (!page) return; /* object not from this slab */
    if (page->use_ptr_next) {
        void *head = page->freelist.head;
        page->freelist.head = obj;
        *((void **)obj) = head;
    } else {
        int idx = (int)((uint8_t *)obj - page->mem) / (int)page->obj_size;
        uint32_t next = (uint32_t)page->freelist.head_index;
        *((uint32_t *)obj) = next;
        page->freelist.head_index = idx;
    }
}

size_t cmq_slab_count(cmq_slab_t *slab) {
    if (!slab) return 0;
    return slab->total_allocated;
}
