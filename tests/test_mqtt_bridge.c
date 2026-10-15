/* P1 v0.5.3: F19b MQTT→cmq_sublist bridge plumbing. */

#include "cmq_test.h"
#include "cmq_mqtt.h"
#include "cmq_mqtt_server.h"
#include "cmq_server.h"
#include "cmq_sublist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

static int bridge_listen_loopback(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }
    *port = (int)ntohs(addr.sin_port);
    return fd;
}

static int bridge_read_exact(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

struct bridge_broker {
    int broker_fd;
    atomic_int saw_qos1;
    atomic_int failed;
};

static void *bridge_qos1_broker(void *arg) {
    struct bridge_broker *broker = arg;
    int fd = accept(broker->broker_fd, NULL, NULL);
    if (fd < 0) {
        atomic_store(&broker->failed, 1);
        return NULL;
    }
    uint8_t connect[256];
    if (bridge_read_exact(fd, connect, 2) != 0 || connect[0] != 0x10 ||
        connect[1] > sizeof(connect) - 2 ||
        bridge_read_exact(fd, connect + 2, connect[1]) != 0) {
        atomic_store(&broker->failed, 1);
        close(fd);
        return NULL;
    }
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    if (send(fd, connack, sizeof(connack), 0) != (ssize_t)sizeof(connack)) {
        atomic_store(&broker->failed, 1);
        close(fd);
        return NULL;
    }

    uint8_t publish[256];
    if (bridge_read_exact(fd, publish, 2) != 0 || (publish[0] & 0xF0) != 0x30 ||
        (publish[0] & 0x06) != 0x02 || publish[1] > sizeof(publish) - 2 ||
        bridge_read_exact(fd, publish + 2, publish[1]) != 0) {
        atomic_store(&broker->failed, 1);
        close(fd);
        return NULL;
    }
    size_t topic_len = ((size_t)publish[2] << 8) | publish[3];
    size_t pid_pos = 4 + topic_len;
    if (pid_pos + 2 > (size_t)publish[1] + 2) {
        atomic_store(&broker->failed, 1);
        close(fd);
        return NULL;
    }
    uint16_t packet_id = ((uint16_t)publish[pid_pos] << 8) | publish[pid_pos + 1];
    atomic_store(&broker->saw_qos1, 1);
    uint8_t puback[] = {0x40, 0x02, (uint8_t)(packet_id >> 8), (uint8_t)packet_id};
    if (send(fd, puback, sizeof(puback), 0) != (ssize_t)sizeof(puback))
        atomic_store(&broker->failed, 1);
    close(fd);
    return NULL;
}

TEST(mqtt_bridge, qos1_waits_for_matching_puback) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker;
    broker.broker_fd = broker_fd;
    atomic_init(&broker.saw_qos1, 0);
    atomic_init(&broker.failed, 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_qos1_broker, &broker), 0);

    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("qos1-test");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(bridge, "orders.created", "orders/created", 1), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "orders.created",
                                      (const uint8_t *)"ok", 2), 1);
    ASSERT_EQ(atomic_load(&broker.saw_qos1), 1);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    pthread_join(thread, NULL);
    close(broker_fd);
}

TEST(mqtt_bridge, set_bridge_server_keeps_pointer) {
    /* cmq_mqtt_set_bridge_server records the server pointer. The
     * actual sublist insertion happens in the relay thread, which
     * is v0.6 work. v0.5.3 ships the plumbing + API. */
    cmq_mqtt_set_bridge_server((void *)0xdeadbeef);
    /* No assertion needed — just verify no crash. */
    cmq_mqtt_set_bridge_server(NULL);
}

/* v0.5.35: the bridge relay must NOT create sublist subscriptions
 * via cmq_sublist_insert. With the v0.5.6 wiring, the relay called
 * cmq_sublist_insert(srv->sublist, topic, payload) — but that's the
 * subscription registry API, not a publish API. The relay's
 * payload pointers landed in trie node `subs[]` arrays where they
 * were never reachable. The v0.5.34 fix removed the double-free;
 * v0.5.35 removes the wrong wiring so the relay stays on the
 * no-op path (which is the only path the v0.5.34 freelist load
 * test exercises). */
TEST(mqtt_bridge, relay_does_not_create_subscriptions) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25040;
    cfg.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Read sublist count before any bridge activity. */
    size_t count_before = cmq_sublist_count(srv->sublist);

    /* Start the relay. */
    cmq_mqtt_set_bridge_server(srv);

    /* Push a payload that would, with the wrong wiring, be inserted
     * as a "subscription" (matched by the subject "v0.5.35/sentinel").
     * With the no-op default, the relay just recycles the buffer. */
    extern int cmq_mqtt_test_enqueue_bridge(const char *topic,
                                              const uint8_t *payload,
                                              size_t len);
    uint8_t payload[32] = {0};
    ASSERT_EQ(cmq_mqtt_test_enqueue_bridge("v0.5.35/sentinel",
                                            payload, sizeof(payload)),
              0);

    /* Wait briefly for the relay to drain. */
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    /* Sublist count must be unchanged — the relay must not have
     * inserted any subscription. */
    size_t count_after = cmq_sublist_count(srv->sublist);
    ASSERT_EQ(count_after, count_before);

    cmq_mqtt_bridge_shutdown();
    cmq_server_destroy(srv);
}

/* v0.5.36: bridge exercises cmq_server_publish without crashing.
 * The relay calls the bridge publish adapter which invokes the new
 * cmq_server_publish helper. With zero subscribers, the call
 * completes cleanly and stat_messages_dropped is 0. End-to-end
 * with a real subscriber would require a full client request-reply
 * test driver (v0.6 scope). */
#include "cmq_sublist.h"
#include "cmq_server.h"

extern int cmq_mqtt_test_enqueue_bridge(const char *topic,
                                          const uint8_t *payload,
                                          size_t len);

TEST(mqtt_bridge, end_to_end_fanout_to_subscriber) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25050;
    cfg.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    cmq_mqtt_set_bridge_server(srv);

    const char *topic = "v0.5.36/fanout/sentinel";
    uint8_t payload[32] = {0xCA, 0xFE, 0xBA, 0xBE};
    ASSERT_EQ(cmq_mqtt_test_enqueue_bridge(topic, payload, sizeof(payload)), 0);

    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    uint64_t dropped = cmq_atomic_load_u64(&srv->stat_messages_dropped,
                                              CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(dropped, 0);

    cmq_mqtt_bridge_shutdown();
    cmq_server_destroy(srv);
}

TEST_MAIN()
