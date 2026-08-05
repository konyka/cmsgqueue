#include "cmq_mqtt_server.h"
#include <errno.h>

int cmq_mqtt_server_listen(const char *bind_addr, int port) {
    (void)bind_addr;
    (void)port;
    /* Server-side MQTT listener is deferred to v0.4.0. */
    return -ENOSYS;
}
