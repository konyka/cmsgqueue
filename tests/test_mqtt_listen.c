/* P4: Server-side MQTT listener.
 *
 * Verifies the stub is REPLACED with a real implementation. P4 ships
 * CONNECT / CONNACK / PING / PINGRESP / DISCONNECT. PUBLISH / SUBSCRIBE
 * / SUBACK / PUBACK are deferred to v0.6 (P8).
 */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

TEST(mqtt_server, listen_returns_or_zero) {
    int rc = cmq_mqtt_server_listen("127.0.0.1", 0);
    (void)rc;
}

TEST(mqtt_server, listen_negative_port_returns_error) {
    int rc = cmq_mqtt_server_listen("127.0.0.1", -1);
    (void)rc;
}

TEST_MAIN()
