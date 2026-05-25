#include "cmq.h"
#include <string.h>

const char *cmq_version(void) {
    return CMQ_VERSION_STRING;
}

cmq_status_t cmq_server_create(cmq_server_t **server, const cmq_config_t *config) {
    (void)server;
    (void)config;
    return CMQ_ERR_UNKNOWN;
}

cmq_status_t cmq_server_run(cmq_server_t *server) {
    (void)server;
    return CMQ_ERR_UNKNOWN;
}

void cmq_server_stop(cmq_server_t *server) {
    (void)server;
}

void cmq_server_destroy(cmq_server_t *server) {
    (void)server;
}
