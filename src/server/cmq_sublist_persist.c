/* F18: Persistent sublist — STUB. */
#include "cmq_sublist_persist.h"
#include <stdlib.h>

struct cmq_sublist_persist { int _placeholder; };

cmq_sublist_persist_t *cmq_sublist_persist_open(const char *dir) {
    (void)dir;
    /* Full implementation deferred to a follow-up. cmq_sublist
     * currently has no WAL integration. */
    return NULL;
}

void cmq_sublist_persist_close(cmq_sublist_persist_t *p) { (void)p; }

int cmq_sublist_persist_record_sub(cmq_sublist_persist_t *p,
                                    uint64_t sub_id, const char *subject,
                                    const char *account) {
    (void)p; (void)sub_id; (void)subject; (void)account;
    return -1;
}

int cmq_sublist_persist_record_unsub(cmq_sublist_persist_t *p,
                                      uint64_t sub_id) {
    (void)p; (void)sub_id;
    return -1;
}

int cmq_sublist_persist_load(cmq_sublist_persist_t *p,
                              cmq_sublist_persist_cb cb, void *ctx) {
    (void)p; (void)cb; (void)ctx;
    return -1;
}
