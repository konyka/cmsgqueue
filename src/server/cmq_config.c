#define _POSIX_C_SOURCE 200809L
#include "cmq_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>

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

static void parse_key_value(const char *key, const char *value, cmq_config_t *config) {
    if (strcmp(key, "host") == 0) {
        free((void *)config->host);
        config->host = strdup(value);
    } else if (strcmp(key, "port") == 0) {
        config->port = atoi(value);
    } else if (strcmp(key, "threads") == 0 || strcmp(key, "num_threads") == 0) {
        config->num_threads = atoi(value);
    } else if (strcmp(key, "max_clients") == 0) {
        config->max_clients = atoi(value);
    } else if (strcmp(key, "max_payload_size") == 0) {
        config->max_payload_size = atoi(value);
    } else if (strcmp(key, "max_subs_per_client") == 0) {
        config->max_subs_per_client = atoi(value);
    } else if (strcmp(key, "ping_interval") == 0 || strcmp(key, "ping_interval_ms") == 0) {
        config->ping_interval_ms = atoi(value);
    } else if (strcmp(key, "write_timeout") == 0 || strcmp(key, "write_timeout_ms") == 0) {
        config->write_timeout_ms = atoi(value);
    } else if (strcmp(key, "log_file") == 0) {
        free((void *)config->log_file);
        config->log_file = strdup(value);
    } else if (strcmp(key, "log_level") == 0) {
        config->log_level = atoi(value);
    } else if (strcmp(key, "log_to_stdout") == 0) {
        config->log_to_stdout = atoi(value);
    } else if (strcmp(key, "log_to_file") == 0) {
        config->log_to_file = atoi(value);
    } else if (strcmp(key, "auth_username") == 0) {
        free((void *)config->auth_username);
        config->auth_username = strdup(value);
    } else if (strcmp(key, "auth_password") == 0) {
        free((void *)config->auth_password);
        config->auth_password = strdup(value);
    } else if (strcmp(key, "cluster_name") == 0) {
        free((void *)config->cluster_name);
        config->cluster_name = strdup(value);
    } else if (strcmp(key, "cluster_node_id") == 0) {
        free((void *)config->cluster_node_id);
        config->cluster_node_id = strdup(value);
    } else if (strcmp(key, "tls_enabled") == 0) {
        config->tls_enabled = atoi(value);
    } else if (strcmp(key, "tls_cert") == 0) {
        free((void *)config->tls_cert);
        config->tls_cert = strdup(value);
    } else if (strcmp(key, "tls_key") == 0) {
        free((void *)config->tls_key);
        config->tls_key = strdup(value);
    } else if (strcmp(key, "route") == 0) {
        if (config->route_count >= 8) return;
        char *colon = strrchr(value, ':');
        if (!colon || colon == value || colon[1] == '\0') return;
        size_t alen = (size_t)(colon - value);
        char *addr = malloc(alen + 1);
        if (!addr) return;
        memcpy(addr, value, alen);
        addr[alen] = '\0';
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            free(addr);
            return;
        }
        config->routes[config->route_count].addr = addr;
        config->routes[config->route_count].port = port;
        config->route_count++;
    }
}

void cmq_config_free(cmq_config_t *config) {
    if (!config) return;
    free((void *)config->host);
    free((void *)config->log_file);
    free((void *)config->auth_username);
    free((void *)config->auth_password);
    free((void *)config->cluster_name);
    free((void *)config->cluster_node_id);
    free((void *)config->tls_cert);
    free((void *)config->tls_key);
    for (int i = 0; i < config->route_count && i < 8; i++)
        free((void *)config->routes[i].addr);
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

    FILE *fp = fopen(path, "r");
    if (!fp) return CMQ_ERR_IO;

    char line[1024];
    int lineno __attribute__((unused)) = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
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

        parse_key_value(key, value, config);
    }

    fclose(fp);
    return CMQ_OK;
}

cmq_status_t cmq_config_validate(const cmq_config_t *config) {
    if (!config) return CMQ_ERR_INVALID_ARG;
    if (config->port < 0 || config->port > 65535) return CMQ_ERR_INVALID_ARG;
    if (config->max_payload_size < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_subs_per_client < 0) return CMQ_ERR_INVALID_ARG;
    if (config->ping_interval_ms < 0) return CMQ_ERR_INVALID_ARG;
    if (config->write_timeout_ms < 0) return CMQ_ERR_INVALID_ARG;
    if (config->max_clients < 0) return CMQ_ERR_INVALID_ARG;
    if (config->num_threads < 0) return CMQ_ERR_INVALID_ARG;
    /* Routes without cluster identity would silently disable forwarding. */
    if (config->route_count > 0 &&
        (!config->cluster_name || !config->cluster_node_id ||
         config->cluster_name[0] == '\0' ||
         config->cluster_node_id[0] == '\0'))
        return CMQ_ERR_INVALID_ARG;
    /* Inbound route identity is by peer IP only — duplicate IPs collide on rN. */
    for (int i = 0; i < config->route_count; i++) {
        if (!config->routes[i].addr) return CMQ_ERR_INVALID_ARG;
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
