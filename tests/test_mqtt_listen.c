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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

TEST(mqtt_server, listen_uses_requested_bind_address_and_port) {
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(blocker >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.2", &addr.sin_addr), 1);
    ASSERT_EQ(bind(blocker, (struct sockaddr *)&addr, sizeof(addr)), 0);

    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(getsockname(blocker, (struct sockaddr *)&addr, &addr_len), 0);
    int port = ntohs(addr.sin_port);
    ASSERT(port > 0);
    ASSERT_EQ(listen(blocker, 1), 0);

    ASSERT_EQ(cmq_mqtt_server_listen("127.0.0.2", port), 0);
    close(blocker);
}

TEST(mqtt_server, listen_rejects_invalid_arguments) {
    int rc = cmq_mqtt_server_listen("127.0.0.1", -1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_mqtt_server_listen("not-an-ip", 1883), 0);
}

TEST(mqtt_server, listen_accepts_ephemeral_port) {
    ASSERT_EQ(cmq_mqtt_server_listen("127.0.0.1", 0), 1);
}

TEST_MAIN()
