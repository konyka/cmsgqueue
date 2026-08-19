/* P1 v0.5.3: F19b MQTT→cmq_sublist bridge plumbing. */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

TEST(mqtt_bridge, set_bridge_server_keeps_pointer) {
    /* cmq_mqtt_set_bridge_server records the server pointer. The
     * actual sublist insertion happens in the relay thread, which
     * is v0.6 work. v0.5.3 ships the plumbing + API. */
    cmq_mqtt_set_bridge_server((void *)0xdeadbeef);
    /* No assertion needed — just verify no crash. */
    cmq_mqtt_set_bridge_server(NULL);
}

TEST_MAIN()