#define _POSIX_C_SOURCE 200809L
#include "cmq_blocklist.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#define CMQ_BLOCKLIST_MAX_ENTRIES 4096

struct cmq_entry {
    sa_family_t family;
    unsigned char address[16];
    unsigned char mask[16];
};

struct cmq_blocklist {
    pthread_mutex_t lock;
    char *path;
    int n;
    struct cmq_entry entries[CMQ_BLOCKLIST_MAX_ENTRIES];
};

static int parse_entry(const char *line, struct cmq_entry *out) {
    char buf[INET6_ADDRSTRLEN + 8];
    size_t n = 0;
    while (line[n] && line[n] != '\n' && line[n] != '\r' && n < sizeof(buf) - 1) {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';
    if (n == 0) return -1;
    char *slash = strchr(buf, '/');
    int prefix;
    if (slash) {
        *slash = '\0';
        char *end = NULL;
        long parsed;
        errno = 0;
        parsed = strtol(slash + 1, &end, 10);
        if (errno != 0 || end == slash + 1 || *end != '\0' ||
            parsed < 0 || parsed > 128)
            return -1;
        prefix = (int)parsed;
    } else {
        prefix = -1;
    }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) == 1) {
        if (prefix < 0) prefix = 32;
        if (prefix > 32) return -1;
        out->family = AF_INET;
        memcpy(out->address, &a, sizeof(a));
        memset(out->mask, 0, sizeof(out->mask));
        for (int bit = 0; bit < prefix; bit++)
            out->mask[bit / 8] |= (unsigned char)(0x80u >> (bit % 8));
        return 0;
    }

    struct in6_addr a6;
    if (inet_pton(AF_INET6, buf, &a6) != 1) return -1;
    if (prefix < 0) prefix = 128;
    if (prefix > 128) return -1;
    out->family = AF_INET6;
    memcpy(out->address, &a6, sizeof(a6));
    memset(out->mask, 0, sizeof(out->mask));
    for (int bit = 0; bit < prefix; bit++)
        out->mask[bit / 8] |= (unsigned char)(0x80u >> (bit % 8));
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

static int blocklist_path_ok(const char *path) {
    if (!path || !path[0]) return 0;
    size_t n = strnlen(path, 512);
    if (n == 0 || n >= 512) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '\\' || c < 0x20 || c == 0x7f)
            return 0;
    }
    size_t i = 0;
    while (i < n) {
        while (i < n && path[i] == '/')
            i++;
        if (i >= n)
            break;
        size_t start = i;
        while (i < n && path[i] != '/')
            i++;
        size_t len = i - start;
        if (len == 1 && path[start] == '.')
            return 0;
        if (len == 2 && path[start] == '.' && path[start + 1] == '.')
            return 0;
    }
    return 1;
}

int cmq_blocklist_reload_attach(cmq_blocklist_t **bl,
                                const char **live_path,
                                const char *fresh_path) {
    if (!bl) return -1;
    if (!fresh_path || !fresh_path[0])
        return 0;
    if (!blocklist_path_ok(fresh_path))
        return -1;
    if (*bl)
        return 0;
    if (access(fresh_path, R_OK) != 0)
        return -1;
    cmq_blocklist_t *n = cmq_blocklist_load(fresh_path);
    if (!n) return -1;
    if (live_path) {
        char *owned = strdup(fresh_path);
        if (!owned) {
            cmq_blocklist_free(n);
            return -1;
        }
        free((void *)(uintptr_t)*live_path);
        *live_path = owned;
    }
    *bl = n;
    return 0;
}

int cmq_blocklist_reload_swap(cmq_blocklist_t **out,
                              const char **live_path,
                              const char *fresh_path) {
    if (!out) return -1;
    if (!fresh_path || !fresh_path[0])
        return 0;
    if (!blocklist_path_ok(fresh_path))
        return -1;
    if (access(fresh_path, R_OK) != 0)
        return -1;
    cmq_blocklist_t *n = cmq_blocklist_load(fresh_path);
    if (!n) return -1;
    if (live_path) {
        if (*live_path && strcmp(*live_path, fresh_path) == 0) {
            *out = n;
            return 0;
        }
        char *owned = strdup(fresh_path);
        if (!owned) {
            cmq_blocklist_free(n);
            return -1;
        }
        free((void *)(uintptr_t)*live_path);
        *live_path = owned;
    }
    *out = n;
    return 0;
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

static int check_address(const cmq_blocklist_t *bl, sa_family_t family,
                         const unsigned char *address, size_t address_len) {
    if (!bl) return 0;
    int blocked = 0;
    pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)&bl->lock);
    for (int i = 0; i < bl->n; i++) {
        const struct cmq_entry *entry = &bl->entries[i];
        if (entry->family != family) continue;
        blocked = 1;
        for (size_t byte = 0; byte < address_len; byte++) {
            if ((address[byte] & entry->mask[byte]) !=
                (entry->address[byte] & entry->mask[byte])) {
                blocked = 0;
                break;
            }
        }
        if (blocked) break;
    }
    pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)&bl->lock);
    return blocked;
}

int cmq_blocklist_check(const cmq_blocklist_t *bl, uint32_t ip_be) {
    unsigned char address[sizeof(ip_be)];
    memcpy(address, &ip_be, sizeof(address));
    return check_address(bl, AF_INET, address, sizeof(address));
}

int cmq_blocklist_check_sockaddr(const cmq_blocklist_t *bl,
                                 const struct sockaddr *addr,
                                 socklen_t addrlen) {
    if (!addr) return 0;
    if (addr->sa_family == AF_INET) {
        if (addrlen < (socklen_t)sizeof(struct sockaddr_in)) return 0;
        struct sockaddr_in in;
        memcpy(&in, addr, sizeof(in));
        return check_address(bl, AF_INET, (const unsigned char *)&in.sin_addr,
                             sizeof(in.sin_addr));
    }
    if (addr->sa_family == AF_INET6) {
        if (addrlen < (socklen_t)sizeof(struct sockaddr_in6)) return 0;
        struct sockaddr_in6 in6;
        memcpy(&in6, addr, sizeof(in6));
        return check_address(bl, AF_INET6,
                             (const unsigned char *)&in6.sin6_addr,
                             sizeof(in6.sin6_addr));
    }
    return 0;
}
