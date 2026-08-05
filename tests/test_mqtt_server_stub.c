/* F19: Server-side MQTT listener stub. */

#include "cmq_test.h"
#include "cmq_mqtt_server.h"
#include <errno.h>
#include <string.h>

TEST(mqtt_server, listen_returns_enosys) {
    int rc = cmq_mqtt_server_listen("0.0.0.0", 1883);
    /* Stub returns -ENOSYS to signal deferred implementation. */
    ASSERT_EQ(rc, -ENOSYS);
}

TEST(mqtt_server, listen_with_null_addr_returns_enosys) {
    int rc = cmq_mqtt_server_listen(NULL, 0);
    ASSERT_EQ(rc, -ENOSYS);
}

TEST_MAIN()
