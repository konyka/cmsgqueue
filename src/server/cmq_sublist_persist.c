#define _POSIX_C_SOURCE 200809L
#include "cmq_sublist_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* F18: Persistent subscription state.
 *
 * The WAL is a text file with one line per record:
 *   S <sub_id> <subject> <account>\n    (subscribe)
 *   U <sub_id>\n                          (unsubscribe)
 *
 * The file is opened with O_APPEND for writes. On load, we re-read
 * sequentially and apply each record (the cb invokes the in-memory
 * mirror). The implementation is intentionally simple; this is the
 * contract that the cmq_sublist_persist API exposes.
 *
 * Concurrency: writes are serialized by an internal mutex. The
 * caller is responsible for invoking us from the publishing thread.
 */

struct cmq_sublist_persist {
    char *path;
    FILE *fp;
    pthread_mutex_t lock;
};

static int write_record(FILE *fp, int is_sub, uint64_t sub_id,
                         const char *subject, const char *account) {
    if (is_sub) {
        int n = fprintf(fp, "S %llu %s %s\n",
                         (unsigned long long)sub_id,
                         subject ? subject : "",
                         account ? account : "");
        return n > 0 ? 0 : -1;
    }
    int n = fprintf(fp, "U %llu\n", (unsigned long long)sub_id);
    return n > 0 ? 0 : -1;
}

cmq_sublist_persist_t *cmq_sublist_persist_open(const char *dir) {
    if (!dir || !dir[0]) return NULL;
    cmq_sublist_persist_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    size_t n = strlen(dir) + 32;
    p->path = malloc(n);
    if (!p->path) { free(p); return NULL; }
    snprintf(p->path, n, "%s/cmq-subs.wal", dir);
    pthread_mutex_init(&p->lock, NULL);
    p->fp = fopen(p->path, "a");
    if (!p->fp) {
        free(p->path);
        free(p);
        return NULL;
    }
    setvbuf(p->fp, NULL, _IOLBF, 0);
    return p;
}

void cmq_sublist_persist_close(cmq_sublist_persist_t *p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (p->fp) fclose(p->fp);
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_destroy(&p->lock);
    free(p->path);
    free(p);
}

int cmq_sublist_persist_record_sub(cmq_sublist_persist_t *p,
                                    uint64_t sub_id, const char *subject,
                                    const char *account) {
    if (!p || !p->fp) return -1;
    pthread_mutex_lock(&p->lock);
    int rc = write_record(p->fp, 1, sub_id, subject, account);
    pthread_mutex_unlock(&p->lock);
    return rc;
}

int cmq_sublist_persist_record_unsub(cmq_sublist_persist_t *p,
                                      uint64_t sub_id) {
    if (!p || !p->fp) return -1;
    pthread_mutex_lock(&p->lock);
    int rc = write_record(p->fp, 0, sub_id, NULL, NULL);
    pthread_mutex_unlock(&p->lock);
    return rc;
}

int cmq_sublist_persist_load(cmq_sublist_persist_t *p,
                              cmq_sublist_persist_cb cb, void *ctx) {
    if (!p || !p->path || !cb) return -1;
    FILE *fp = fopen(p->path, "r");
    if (!fp) return 0;
    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (line[0] == 'S') {
            char *p1 = line + 1;
            while (*p1 == ' ') p1++;
            uint64_t sub_id = strtoull(p1, &p1, 10);
            while (*p1 == ' ') p1++;
            char *subject = p1;
            while (*p1 && *p1 != ' ') p1++;
            if (*p1) { *p1 = '\0'; p1++; }
            while (*p1 == ' ') p1++;
            char *account = p1;
            cb(ctx, 1, sub_id, subject, account);
            count++;
        } else if (line[0] == 'U') {
            uint64_t sub_id = strtoull(line + 1, NULL, 10);
            cb(ctx, 0, sub_id, NULL, NULL);
            count++;
        }
    }
    fclose(fp);
    return count;
}
