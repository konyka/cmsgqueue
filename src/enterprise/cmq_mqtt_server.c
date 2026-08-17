#include "cmq_mqtt_server.h"
#include <errno.h>

int cmq_mqtt_server_listen(const char *bind_addr, int port) {
    (void)bind_addr;
    (void)port;
    /* F19: full implementation deferred to v0.5.5. The API returns
     * 0 so callers can probe availability. */
    return 0;
}

void cmq_mqtt_server_start_listener(struct cmq_server *server) {
    (void)server;
    /* Stub. */
}
