#define _POSIX_C_SOURCE 200809L
#include "cmq_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void strip_whitespace(char *s) {
    char *dst = s;
    while (*s) {
        if (!isspace((unsigned char)*s)) {
            *dst++ = *s;
        }
        s++;
    }
    *dst = '\0';
}

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
        config->host = strdup(value);
    } else if (strcmp(key, "port") == 0) {
        config->port = atoi(value);
    } else if (strcmp(key, "threads") == 0 || strcmp(key, "num_threads") == 0) {
        config->num_threads = atoi(value);
    } else if (strcmp(key, "max_clients") == 0) {
        config->max_clients = atoi(value);
    } else if (strcmp(key, "max_payload_size") == 0) {
        config->max_payload_size = atoi(value);
    } else if (strcmp(key, "ping_interval") == 0 || strcmp(key, "ping_interval_ms") == 0) {
        config->ping_interval_ms = atoi(value);
    } else if (strcmp(key, "write_timeout") == 0 || strcmp(key, "write_timeout_ms") == 0) {
        config->write_timeout_ms = atoi(value);
    } else if (strcmp(key, "log_file") == 0) {
        config->log_file = strdup(value);
    } else if (strcmp(key, "log_level") == 0) {
        config->log_level = atoi(value);
    } else if (strcmp(key, "log_to_stdout") == 0) {
        config->log_to_stdout = atoi(value);
    } else if (strcmp(key, "log_to_file") == 0) {
        config->log_to_file = atoi(value);
    }
}

cmq_status_t cmq_config_load(const char *path, cmq_config_t *config) {
    if (!path || !config) return CMQ_ERR_INVALID_ARG;

    FILE *fp = fopen(path, "r");
    if (!fp) return CMQ_ERR_IO;

    char line[1024];
    int lineno = 0;
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
    if (config->ping_interval_ms < 0) return CMQ_ERR_INVALID_ARG;
    if (config->num_threads < 0) return CMQ_ERR_INVALID_ARG;
    return CMQ_OK;
}
