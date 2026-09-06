#define _POSIX_C_SOURCE 200809L
#include "cmq_dynreload.h"
#include "cmq_acl.h"
#include <stdlib.h>
#include <string.h>

static int apply_csv(cmq_acl_t *acl, int allow, const char *csv) {
    if (!csv || !csv[0]) return 0;
    char *copy = strdup(csv);
    if (!copy) return -1;
    char *save = NULL;
    int rc = 0;
    for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (allow) {
            if (cmq_acl_allow(acl, t) != 0) {
                rc = -1;
                break;
            }
        } else if (cmq_acl_deny(acl, t) != 0) {
            rc = -1;
            break;
        }
    }
    free(copy);
    return rc;
}

int cmq_reload_apply_dynamic(cmq_log_t *log, int *log_level,
                             cmq_rch_t **acl_h,
                             const cmq_config_t *fresh) {
    if (!fresh || !log_level) return -1;
    if (fresh->log_level < 0 || fresh->log_level > 5) return -1;
    if (log)
        cmq_log_set_level(log, (cmq_log_level_t)fresh->log_level);
    *log_level = fresh->log_level;

    if (!acl_h || (!fresh->acl_allow && !fresh->acl_deny))
        return 0;

    cmq_acl_t *new_acl = cmq_acl_create();
    if (!new_acl) return -1;
    if (apply_csv(new_acl, 1, fresh->acl_allow) != 0 ||
        apply_csv(new_acl, 0, fresh->acl_deny) != 0) {
        cmq_acl_free(new_acl);
        return -1;
    }

    if (*acl_h) {
        cmq_acl_t *cur = (cmq_acl_t *)cmq_rch_acquire(*acl_h);
        int cur_probe = cur ? cmq_acl_check(cur, "_probe_") : 1;
        cmq_rch_release(*acl_h, cur);
        if (cmq_acl_check(new_acl, "_probe_") != cur_probe) {
            cmq_acl_free(new_acl);
            return 0;
        }
    }

    cmq_rch_t *nh = cmq_rch_new(new_acl, (cmq_rch_free_fn)cmq_acl_free);
    if (!nh) {
        cmq_acl_free(new_acl);
        return -1;
    }
    if (*acl_h) {
        cmq_rch_t *old = cmq_rch_swap(acl_h, nh);
        if (old) cmq_rch_release_owner(old);
    } else {
        *acl_h = nh;
    }
    return 0;
}
