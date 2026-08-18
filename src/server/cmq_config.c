#define _POSIX_C_SOURCE 200809L
#include "cmq_config.h"
#include "cmq_cluster.h"
#include "cmq_account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Matches cmq_cluster.name[64] in cmq_cluster_create. */
#define CMQ_CLUSTER_NAME_MAX 64

static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    size_t len = strlen(start);
    if (len == 0) {
        *s = '\0';
        return;
    }
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    memmove(s, start, len);
    s[len] = '\0';
}

static void strip_comments(char *line) {
    int in_string = 0;
    char *p = line;
    while (*p) {
        if (*p == '"' && (p == line || *(p - 1) != '\\')) {
            in_string = !in_string;
        } else if (*p == '#' && !in_string) {
            *p = '\0';
            return;
        }
        p++;
    }
}

static void cfg_free_owned(const char *ptr) {
    free((void *)(uintptr_t)ptr);
}

static int cfg_set_str(const char **dst, const char *value) {
    char *copy = strdup(value);
    if (!copy) return -1;
    cfg_free_owned(*dst);
    *dst = copy;
    return 0;
}

static int cfg_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Names (README) or 0..5 - atoi("info") is 0 and must not mean TRACE. */
static int parse_log_level(const char *value, int *out) {
    if (!value || !out) return -1;
    if (cfg_eq_ci(value, "trace")) { *out = 0; return 0; }
    if (cfg_eq_ci(value, "debug")) { *out = 1; return 0; }
    if (cfg_eq_ci(value, "info"))  { *out = 2; return 0; }
    if (cfg_eq_ci(value, "warn") || cfg_eq_ci(value, "warning")) {
        *out = 3;
        return 0;
    }
    if (cfg_eq_ci(value, "error")) { *out = 4; return 0; }
    if (cfg_eq_ci(value, "fatal")) { *out = 5; return 0; }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (!end || end == value || *end != '\0' || v < 0 || v > 5)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_int_range(const char *value, int min, int max, int *out) {
    if (!value || !out || min > max) return -1;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (!end || end == value || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_key_value(const char *key, const char *value, cmq_config_t *config) {
    if (strcmp(key, "host") == 0) {
        return cfg_set_str(&config->host, value);
    } else if (strcmp(key, "port") == 0) {
        return parse_int_range(value, 0, 65535, &config->port);
    } else if (strcmp(key, "threads") == 0 || strcmp(key, "num_threads") == 0) {
        return parse_int_range(value, 0, 64, &config->num_threads);
    } else if (strcmp(key, "max_clients") == 0) {
        return parse_int_range(value, 0, CMQ_MAX_CLIENTS_LIMIT, &config->max_clients);
    } else if (strcmp(key, "max_payload_size") == 0) {
        return parse_int_range(value, 0, CMQ_MAX_PAYLOAD_LIMIT, &config->max_payload_size);
    } else if (strcmp(key, "max_subs_per_client") == 0) {
        return parse_int_range(value, 0, CMQ_DEFAULT_MAX_SUBS_PER_CLIENT,
                               &config->max_subs_per_client);
    } else if (strcmp(key, "max_connects_per_sec") == 0) {
        return parse_int_range(value, 0, 100000, &config->max_connects_per_sec);
    } else if (strcmp(key, "max_msgs_per_sec_per_account") == 0) {
        return parse_int_range(value, 0, 10000000,
                              &config->max_msgs_per_sec_per_account);
    } else if (strcmp(key, "max_bytes_per_sec_per_account") == 0) {
        return parse_int_range(value, 0, 1073741824,
                              &config->max_bytes_per_sec_per_account);
    } else if (strcmp(key, "max_connections_per_account") == 0) {
        return parse_int_range(value, 0, 1000000,
                              &config->max_connections_per_account);
    } else if (strcmp(key, "max_msgs_per_sec_per_subject") == 0) {
        return parse_int_range(value, 0, 1000000,
                              &config->max_msgs_per_sec_per_subject);
    } else if (strcmp(key, "acl_allow") == 0) {
        return cfg_set_str(&config->acl_allow, value);
    } else if (strcmp(key, "acl_deny") == 0) {
        return cfg_set_str(&config->acl_deny, value);
    } else if (strcmp(key, "blocklist_file") == 0) {
        return cfg_set_str(&config->blocklist_file, value);
    } else if (strcmp(key, "inbox_max_pending") == 0) {
        return parse_int_range(value, 0, 100000, &config->inbox_max_pending);
    } else if (strcmp(key, "ping_interval") == 0 || strcmp(key, "ping_interval_ms") == 0) {
        return parse_int_range(value, 0, 86400000, &config->ping_interval_ms);
    } else if (strcmp(key, "write_timeout") == 0 || strcmp(key, "write_timeout_ms") == 0) {
        return parse_int_range(value, 0, 86400000, &config->write_timeout_ms);
    } else if (strcmp(key, "log_file") == 0) {
        return cfg_set_str(&config->log_file, value);
    } else if (strcmp(key, "log_level") == 0) {
        return parse_log_level(value, &config->log_level);
    } else if (strcmp(key, "log_to_stdout") == 0) {
        return parse_int_range(value, 0, 1, &config->log_to_stdout);
    } else if (strcmp(key, "log_to_file") == 0) {
        return parse_int_range(value, 0, 1, &config->log_to_file);
    } else if (strcmp(key, "auth_username") == 0) {
        return cfg_set_str(&config->auth_username, value);
    } else if (strcmp(key, "auth_password") == 0) {
        return cfg_set_str(&config->auth_password, value);
    } else if (strcmp(key, "cluster_name") == 0) {
        return cfg_set_str(&config->cluster_name, value);
    } else if (strcmp(key, "cluster_node_id") == 0) {
        return cfg_set_str(&config->cluster_node_id, value);
    } else if (strcmp(key, "tls_enabled") == 0) {
        return parse_int_range(value, 0, 1, &config->tls_enabled);
    } else if (strcmp(key, "tls_cert") == 0) {
        return cfg_set_str(&config->tls_cert, value);
    } else if (strcmp(key, "tls_key") == 0) {
        return cfg_set_str(&config->tls_key, value);
    } else if (strcmp(key, "tls_ca") == 0) {
        return cfg_set_str(&config->tls_ca, value);
    } else if (strcmp(key, "tls_verify_peer") == 0) {
        return parse_int_range(value, 0, 1, &config->tls_verify_peer);
    } else if (strcmp(key, "route") == 0) {
        if (config->route_count >= 8) return -1;
        char *colon = strrchr(value, ':');
        if (!colon || colon == value || colon[1] == '\0') return -1;
        size_t alen = (size_t)(colon - value);
        char *addr = malloc(alen + 1);
        if (!addr) return -1;
        memcpy(addr, value, alen);
        addr[alen] = '\0';
        int port = 0;
        if (parse_int_range(colon + 1, 1, 65535, &port) != 0) {
            free(addr);
            return -1;
        }
        config->routes[config->route_count].addr = addr;
        config->routes[config->route_count].port = port;
        config->route_count++;
    }
    return 0;
}

void cmq_config_free(cmq_config_t *config) {
    if (!config) return;
    cfg_free_owned(config->host);
    cfg_free_owned(config->log_file);
    cfg_free_owned(config->auth_username);
    cfg_free_owned(config->auth_password);
    cfg_free_owned(config->cluster_name);
    cfg_free_owned(config->cluster_node_id);
    cfg_free_owned(config->tls_cert);
    cfg_free_owned(config->tls_key);
    for (int i = 0; i < config->route_count && i < 8; i++)
        cfg_free_owned(config->routes[i].addr);
    config->host = NULL;
    config->log_file = NULL;
    config->auth_username = NULL;
    config->auth_password = NULL;
    config->cluster_name = NULL;
    config->cluster_node_id = NULL;
    config->tls_cert = NULL;
    config->tls_key = NULL;
    for (int i = 0; i < 8; i++) {
        config->routes[i].addr = NULL;
        config->routes[i].port = 0;
    }
    config->route_count = 0;
}

cmq_status_t cmq_config_load(const char *path, cmq_config_t *config) {
    if (!path || !config) return CMQ_ERR_INVALID_ARG;

    /* Reset so unspecified keys are never left as caller stack garbage.
       Free first so reload does not leak prior strdup'd strings (config must
       be zeroed or previously load/free'd — same contract as error paths). */
    cmq_config_free(config);
    memset(config, 0, sizeof(*config));
    /* Omitted log_level key → INFO (0 is TRACE when explicitly set). */
    config->log_level = 2;

    FILE *fp = fopen(path, "r");
    if (!fp) return CMQ_ERR_IO;

    char line[1024];
    int lineno __attribute__((unused)) = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        /* Refuse silently truncated lines (long cert paths / passwords / routes). */
        size_t llen = strlen(line);
        if (llen == sizeof(line) - 1 && line[llen - 1] != '\n') {
            int ch = fgetc(fp);
            if (ch != EOF) {
                fclose(fp);
                cmq_config_free(config);
                return CMQ_ERR_INVALID_ARG;
            }
        }
        strip_comments(line);
        trim(line);
        if (line[0] == '\0' || line[0] == '[') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        trim(key);
        trim(value);

        if (value[0] == '"') {
            size_t vlen = strlen(value);
            if (vlen >= 2 && value[vlen - 1] == '"') {
                memmove(value, value + 1, vlen - 2);
                value[vlen - 2] = '\0';
            }
        }

        if (parse_key_value(key, value, config) != 0) {
            fclose(fp);
            cmq_config_free(config);
            return CMQ_ERR_INVALID_ARG;
        }
    }

    fclose(fp);
    return CMQ_OK;
}

cmq_status_t cmq_config_validate(const cmq_config_t *config) {
    if (!config) return CMQ_ERR_INVALID_ARG;
    if (config->port < 0 || config->port > 65535) return CMQ_ERR_INVALID_ARG;
    if (config->max_payload_size < 0) return CMQ_ERR_INVALID_ARG;
    /* Must fit CMQ_WRITE_BUF_LIMIT after framing — else deliver force-closes. */
    if (config->max_payload_size > CMQ_MAX_PAYLOAD_LIMIT)
        return CMQ_ERR_INVALID_ARG;
    if (config->max_subs_per_client < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_subs_per_client > CMQ_DEFAULT_MAX_SUBS_PER_CLIENT)
        return CMQ_ERR_INVALID_ARG;
    if (config->max_connects_per_sec < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_connects_per_sec > 100000) return CMQ_ERR_INVALID_ARG;
    if (config->inbox_max_pending < 0) return CMQ_ERR_INVALID_ARG;
    if (config->inbox_max_pending > 100000) return CMQ_ERR_INVALID_ARG;
    if (config->max_msgs_per_sec_per_account < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_bytes_per_sec_per_account < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_connections_per_account < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_msgs_per_sec_per_subject < 0) return CMQ_ERR_INVALID_ARG;
    if (config->ping_interval_ms < 0) return CMQ_ERR_INVALID_ARG;
    /* Cap so keepalive timeout_ms = interval*2 cannot overflow int. */
    if (config->ping_interval_ms > 86400000) /* 24h */
        return CMQ_ERR_INVALID_ARG;
    if (config->write_timeout_ms < 0) return CMQ_ERR_INVALID_ARG;
    if (config->write_timeout_ms > 86400000)
        return CMQ_ERR_INVALID_ARG;
    if (config->max_clients < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_clients > CMQ_MAX_CLIENTS_LIMIT)
        return CMQ_ERR_INVALID_ARG;
    if (config->num_threads < 0 || config->num_threads > 64)
        return CMQ_ERR_INVALID_ARG;
    /* Empty auth strings would enable a zero-credential bypass; reject. */
    if (config->auth_username && config->auth_username[0] == '\0')
        return CMQ_ERR_INVALID_ARG;
    if (config->auth_password && config->auth_password[0] == '\0')
        return CMQ_ERR_INVALID_ARG;
    /* Username without password accepts any password — fail closed.
       Password-only (no username) remains valid for shared-secret auth. */
    if (config->auth_username && config->auth_username[0] &&
        (!config->auth_password || !config->auth_password[0]))
        return CMQ_ERR_INVALID_ARG;
    /* Username becomes account name — must fit CMQ_ACCOUNT_NAME_SIZE.
       Password still compared in 256-byte CONNECT pads. */
    if (config->auth_username &&
        strnlen(config->auth_username, CMQ_ACCOUNT_NAME_SIZE) >=
            CMQ_ACCOUNT_NAME_SIZE)
        return CMQ_ERR_INVALID_ARG;
    if (config->auth_password &&
        strnlen(config->auth_password, 256) >= 256)
        return CMQ_ERR_INVALID_ARG;
    /* tls_enabled without cert/key would listen in plaintext — fail closed. */
    if (config->tls_enabled) {
        if (!config->tls_cert || !config->tls_key ||
            config->tls_cert[0] == '\0' || config->tls_key[0] == '\0')
            return CMQ_ERR_INVALID_ARG;
    }
    {
        const char *host = config->host ? config->host : CMQ_DEFAULT_HOST;
        struct in_addr ha;
        if (inet_pton(AF_INET, host, &ha) != 1)
            return CMQ_ERR_INVALID_ARG;
    }
    /* Routes without cluster identity would silently disable forwarding. */
    if (config->route_count > 0 &&
        (!config->cluster_name || !config->cluster_node_id ||
         config->cluster_name[0] == '\0' ||
         config->cluster_node_id[0] == '\0'))
        return CMQ_ERR_INVALID_ARG;
    /* strncpy into fixed pads - reject so IDs cannot collide after truncate. */
    if (config->cluster_node_id &&
        strnlen(config->cluster_node_id, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE)
        return CMQ_ERR_INVALID_ARG;
    if (config->cluster_name &&
        strnlen(config->cluster_name, CMQ_CLUSTER_NAME_MAX) >= CMQ_CLUSTER_NAME_MAX)
        return CMQ_ERR_INVALID_ARG;
    /* routes[8] — reject oversize so validate cannot walk past the array
       into route_count/tls_* (programmatic configs bypass load's i<8 gate). */
    if (config->route_count < 0 || config->route_count > 8)
        return CMQ_ERR_INVALID_ARG;
    /* Inbound route identity is by peer IP only — duplicate IPs collide on rN. */
    for (int i = 0; i < config->route_count; i++) {
        if (!config->routes[i].addr) return CMQ_ERR_INVALID_ARG;
        if (config->routes[i].port <= 0 || config->routes[i].port > 65535)
            return CMQ_ERR_INVALID_ARG;
        struct in_addr a;
        if (inet_pton(AF_INET, config->routes[i].addr, &a) != 1)
            return CMQ_ERR_INVALID_ARG;
        for (int j = 0; j < i; j++) {
            struct in_addr b;
            if (inet_pton(AF_INET, config->routes[j].addr, &b) != 1)
                return CMQ_ERR_INVALID_ARG;
            if (a.s_addr == b.s_addr)
                return CMQ_ERR_INVALID_ARG;
        }
    }
    return CMQ_OK;
}
