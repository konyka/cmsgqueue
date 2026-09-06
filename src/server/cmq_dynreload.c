#define _POSIX_C_SOURCE 200809L
#include "cmq_dynreload.h"
#include "cmq_acl.h"
#include "cmq_atomic.h"
#include <stdlib.h>
#include <string.h>

static cmq_atomic_int cmq_sighup_pending;
static int reload_path_ok(const char *path);

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

static void tls_fresh_paths(const cmq_config_t *fresh, int i,
                            const char **cert, const char **key,
                            const char **ca, int *verify) {
    if (i == 0) {
        *cert = fresh->tls_cert;
        *key = fresh->tls_key;
        *ca = fresh->tls_ca;
        *verify = fresh->tls_verify_peer;
    } else {
        *cert = fresh->listeners[i].tls_cert;
        *key = fresh->listeners[i].tls_key;
        *ca = fresh->listeners[i].tls_ca;
        *verify = fresh->listeners[i].tls_verify_peer;
    }
}

int cmq_reload_apply_tls(cmq_tls_config_t **slots, int nslots,
                         const cmq_config_t *fresh) {
    if (!slots || !fresh || nslots < 0 || nslots > 4) return -1;
    for (int i = 0; i < nslots; i++) {
        if (!slots[i]) continue;
        const char *cert, *key, *ca;
        int verify;
        tls_fresh_paths(fresh, i, &cert, &key, &ca, &verify);
        if (cert && cert[0] && !reload_path_ok(cert)) return -1;
        if (key && key[0] && !reload_path_ok(key)) return -1;
        if (ca && ca[0] && !reload_path_ok(ca)) return -1;
    }
    for (int i = 0; i < nslots; i++) {
        if (!slots[i]) continue;
        const char *cert, *key, *ca;
        int verify;
        tls_fresh_paths(fresh, i, &cert, &key, &ca, &verify);
        if (cert && cert[0] && cmq_tls_set_cert(slots[i], cert) != 0)
            return -1;
        if (key && key[0] && cmq_tls_set_key(slots[i], key) != 0)
            return -1;
        if (ca && ca[0] && cmq_tls_set_ca(slots[i], ca) != 0)
            return -1;
        if (verify)
            (void)cmq_tls_set_verify(slots[i], 1);
        if (cmq_tls_reload(slots[i]) != 0)
            return -1;
    }
    return 0;
}

static int tls_live_dup(const char *fresh, char **out) {
    if (!fresh || !fresh[0]) {
        *out = NULL;
        return 0;
    }
    if (!reload_path_ok(fresh))
        return -1;
    *out = strdup(fresh);
    return *out ? 0 : -1;
}

static int tls_live_take(const char **dst, char *neu) {
    if (!neu)
        return 0;
    if (*dst && strcmp(*dst, neu) == 0) {
        free(neu);
        return 0;
    }
    free((void *)*dst);
    *dst = neu;
    return 0;
}

int cmq_reload_apply_tls_live(cmq_config_t *live, const cmq_config_t *fresh) {
    if (!live || !fresh) return -1;
    char *c0 = NULL, *k0 = NULL, *a0 = NULL;
    char *c1 = NULL, *k1 = NULL, *a1 = NULL;
    char *c2 = NULL, *k2 = NULL, *a2 = NULL;
    char *c3 = NULL, *k3 = NULL, *a3 = NULL;
    if (tls_live_dup(fresh->tls_cert, &c0) != 0 ||
        tls_live_dup(fresh->tls_key, &k0) != 0 ||
        tls_live_dup(fresh->tls_ca, &a0) != 0 ||
        tls_live_dup(fresh->listeners[1].tls_cert, &c1) != 0 ||
        tls_live_dup(fresh->listeners[1].tls_key, &k1) != 0 ||
        tls_live_dup(fresh->listeners[1].tls_ca, &a1) != 0 ||
        tls_live_dup(fresh->listeners[2].tls_cert, &c2) != 0 ||
        tls_live_dup(fresh->listeners[2].tls_key, &k2) != 0 ||
        tls_live_dup(fresh->listeners[2].tls_ca, &a2) != 0 ||
        tls_live_dup(fresh->listeners[3].tls_cert, &c3) != 0 ||
        tls_live_dup(fresh->listeners[3].tls_key, &k3) != 0 ||
        tls_live_dup(fresh->listeners[3].tls_ca, &a3) != 0) {
        free(c0); free(k0); free(a0);
        free(c1); free(k1); free(a1);
        free(c2); free(k2); free(a2);
        free(c3); free(k3); free(a3);
        return -1;
    }
    tls_live_take(&live->tls_cert, c0);
    tls_live_take(&live->tls_key, k0);
    tls_live_take(&live->tls_ca, a0);
    tls_live_take(&live->listeners[1].tls_cert, c1);
    tls_live_take(&live->listeners[1].tls_key, k1);
    tls_live_take(&live->listeners[1].tls_ca, a1);
    tls_live_take(&live->listeners[2].tls_cert, c2);
    tls_live_take(&live->listeners[2].tls_key, k2);
    tls_live_take(&live->listeners[2].tls_ca, a2);
    tls_live_take(&live->listeners[3].tls_cert, c3);
    tls_live_take(&live->listeners[3].tls_key, k3);
    tls_live_take(&live->listeners[3].tls_ca, a3);
    if (fresh->tls_verify_peer)
        live->tls_verify_peer = 1;
    if (fresh->listeners[1].tls_verify_peer)
        live->listeners[1].tls_verify_peer = 1;
    if (fresh->listeners[2].tls_verify_peer)
        live->listeners[2].tls_verify_peer = 1;
    if (fresh->listeners[3].tls_verify_peer)
        live->listeners[3].tls_verify_peer = 1;
    return 0;
}

static int auth_dup(const char *fresh, char **out) {
    if (!fresh || !fresh[0]) {
        *out = NULL;
        return 0;
    }
    *out = strdup(fresh);
    return *out ? 0 : -1;
}

int cmq_reload_apply_auth(cmq_config_t *live, const cmq_config_t *fresh) {
    if (!live || !fresh) return -1;
    if (fresh->jwt_leeway_sec < 0 || fresh->jwt_leeway_sec > 3600)
        return -1;

    char *username = NULL, *password = NULL, *issuer = NULL;
    char *hmac = NULL, *nkey = NULL, *ec = NULL, *rsan = NULL, *rsae = NULL;
    if (auth_dup(fresh->auth_username, &username) != 0 ||
        auth_dup(fresh->auth_password, &password) != 0 ||
        auth_dup(fresh->jwt_issuer, &issuer) != 0 ||
        auth_dup(fresh->jwt_hmac_secret, &hmac) != 0 ||
        auth_dup(fresh->nkey_pub, &nkey) != 0 ||
        auth_dup(fresh->jwt_ec_pub, &ec) != 0 ||
        auth_dup(fresh->jwt_rsa_n, &rsan) != 0 ||
        auth_dup(fresh->jwt_rsa_e, &rsae) != 0) {
        free(username); free(password); free(issuer); free(hmac);
        free(nkey); free(ec); free(rsan); free(rsae);
        return -1;
    }

#define TAKE(dst, neu) do { \
        if (neu) { \
            free((void *)(dst)); \
            (dst) = neu; \
        } \
    } while (0)
    TAKE(live->auth_username, username);
    TAKE(live->auth_password, password);
    TAKE(live->jwt_issuer, issuer);
    TAKE(live->jwt_hmac_secret, hmac);
    TAKE(live->nkey_pub, nkey);
    TAKE(live->jwt_ec_pub, ec);
    TAKE(live->jwt_rsa_n, rsan);
    TAKE(live->jwt_rsa_e, rsae);
#undef TAKE
    if (fresh->jwt_leeway_sec > 0)
        live->jwt_leeway_sec = fresh->jwt_leeway_sec;
    return 0;
}

int cmq_reload_apply_caps(cmq_config_t *live, const cmq_config_t *fresh) {
    if (!live || !fresh) return -1;
    if (fresh->max_payload_size < 0 ||
        fresh->max_payload_size > CMQ_MAX_PAYLOAD_LIMIT)
        return -1;
    if (fresh->max_subs_per_client < 0 ||
        fresh->max_subs_per_client > CMQ_DEFAULT_MAX_SUBS_PER_CLIENT)
        return -1;
    if (fresh->max_clients < 0 ||
        fresh->max_clients > CMQ_MAX_CLIENTS_LIMIT)
        return -1;
    if (fresh->max_payload_size > 0)
        live->max_payload_size = fresh->max_payload_size;
    if (fresh->max_subs_per_client > 0)
        live->max_subs_per_client = fresh->max_subs_per_client;
    if (fresh->max_clients > 0)
        live->max_clients = fresh->max_clients;
    return 0;
}

/* Same fail-closed rules as persist_dir_ok in cmq_config.c. */
static int reload_path_ok(const char *path) {
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

int cmq_reload_apply_config_file(const char **live, const char *fresh) {
    if (!live) return -1;
    if (!fresh || !fresh[0])
        return 0;
    if (*live && strcmp(*live, fresh) == 0)
        return 0;
    if (!reload_path_ok(fresh))
        return -1;
    char *owned = strdup(fresh);
    if (!owned)
        return -1;
    free((void *)*live);
    *live = owned;
    return 0;
}

int cmq_reload_apply_limits(cmq_config_t *live, const cmq_config_t *fresh) {
    if (!live || !fresh) return -1;
    if (fresh->max_connects_per_sec < 0 ||
        fresh->max_connects_per_sec > 100000)
        return -1;
    if (fresh->inbox_max_pending < 0 ||
        fresh->inbox_max_pending > 100000)
        return -1;
    if (fresh->ping_interval_ms < 0 ||
        fresh->ping_interval_ms > 86400000)
        return -1;
    if (fresh->write_timeout_ms < 0 ||
        fresh->write_timeout_ms > 86400000)
        return -1;
    if (fresh->max_connects_per_sec > 0)
        live->max_connects_per_sec = fresh->max_connects_per_sec;
    if (fresh->inbox_max_pending > 0)
        live->inbox_max_pending = fresh->inbox_max_pending;
    if (fresh->ping_interval_ms > 0)
        live->ping_interval_ms = fresh->ping_interval_ms;
    if (fresh->write_timeout_ms > 0)
        live->write_timeout_ms = fresh->write_timeout_ms;
    return 0;
}

void cmq_sighup_note(void) {
    cmq_atomic_store_int(&cmq_sighup_pending, 1, CMQ_ATOMIC_RELEASE);
}

int cmq_sighup_take(void) {
    int expected = 1;
    return cmq_atomic_cas_int(&cmq_sighup_pending, &expected, 0,
                              CMQ_ATOMIC_ACQ_REL) ? 1 : 0;
}
