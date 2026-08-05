#define _POSIX_C_SOURCE 200809L
#include "cmq_blocklist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>

#define CMQ_BLOCKLIST_MAX_ENTRIES 4096

struct cmq_entry {
    uint32_t ip_be;       /* network byte order */
    uint32_t mask_be;     /* network byte order; 0xFFFFFFFF for /32 */
    int has_mask;
};

struct cmq_blocklist {
    pthread_mutex_t lock;
    char *path;
    int n;
    struct cmq_entry entries[CMQ_BLOCKLIST_MAX_ENTRIES];
};

static int parse_entry(const char *line, struct cmq_entry *out) {
    char buf[64];
    size_t n = 0;
    while (line[n] && line[n] != '\n' && line[n] != '\r' && n < sizeof(buf) - 1) {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';
    if (n == 0) return -1;
    char *slash = strchr(buf, '/');
    int prefix = 32;
    if (slash) {
        *slash = '\0';
        prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32) return -1;
    }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) return -1;
    out->ip_be = (uint32_t)a.s_addr;
    out->has_mask = 1;
    out->mask_be = (prefix == 0) ? 0u : htonl(0xFFFFFFFFu << (32 - prefix));
    return 0;
}

cmq_blocklist_t *cmq_blocklist_load(const char *path) {
    if (!path) return NULL;
    cmq_blocklist_t *bl = calloc(1, sizeof(*bl));
    if (!bl) return NULL;
    pthread_mutex_init(&bl->lock, NULL);
    bl->path = strdup(path);
    FILE *f = fopen(path, "r");
    if (!f) { bl->n = 0; return bl; }
    char line[128];
    while (fgets(line, sizeof(line), f) && bl->n < CMQ_BLOCKLIST_MAX_ENTRIES) {
        if (parse_entry(line, &bl->entries[bl->n]) == 0) bl->n++;
    }
    fclose(f);
    return bl;
}

int cmq_blocklist_reload(cmq_blocklist_t *bl, const char *path) {
    if (!bl) return -1;
    cmq_blocklist_t *fresh = cmq_blocklist_load(path);
    if (!fresh) return -1;
    pthread_mutex_lock(&bl->lock);
    memcpy(bl->entries, fresh->entries, sizeof(bl->entries));
    bl->n = fresh->n;
    pthread_mutex_unlock(&bl->lock);
    cmq_blocklist_free(fresh);
    return 0;
}

void cmq_blocklist_free(cmq_blocklist_t *bl) {
    if (!bl) return;
    pthread_mutex_destroy(&bl->lock);
    free(bl->path);
    free(bl);
}

int cmq_blocklist_check(const cmq_blocklist_t *bl, uint32_t ip_be) {
    if (!bl) return 0;
    /* Snapshot n and entries: reload swaps the array under lock.
     * A read here may see a torn view if reload is in flight, but
     * accept_cb is the only caller and accept_cb holds the same
     * lock when needed (via srv->blocklist_lock; see wiring). */
    int n = bl->n;
    for (int i = 0; i < n; i++) {
        if ((ip_be & bl->entries[i].mask_be) == bl->entries[i].ip_be) return 1;
    }
    return 0;
}
