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
#include <poll.h>
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
    int puback_mode;
    atomic_int saw_qos1;
    atomic_int failed;
};

struct forward_broker {
    int broker_fd;
    const char *expected_topic;
    atomic_int saw_publish;
    atomic_int failed;
};

static void *bridge_forward_broker(void *arg) {
    struct forward_broker *broker = arg;
    int fd = accept(broker->broker_fd, NULL, NULL);
    if (fd < 0) {
        atomic_store(&broker->failed, 1);
        return NULL;
    }
    uint8_t frame[256];
    if (bridge_read_exact(fd, frame, 2) != 0 || frame[0] != 0x10 ||
        frame[1] > sizeof(frame) - 2 ||
        bridge_read_exact(fd, frame + 2, frame[1]) != 0) {
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
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 250) > 0 && (pfd.revents & POLLIN)) {
        if (bridge_read_exact(fd, frame, 2) != 0 ||
            (frame[0] & 0xF0) != 0x30 || frame[1] > sizeof(frame) - 2 ||
            bridge_read_exact(fd, frame + 2, frame[1]) != 0) {
            atomic_store(&broker->failed, 1);
        } else {
            size_t topic_len = ((size_t)frame[2] << 8) | frame[3];
            if (topic_len >= sizeof(frame) - 4 ||
                !broker->expected_topic ||
                strlen(broker->expected_topic) != topic_len ||
                memcmp(frame + 4, broker->expected_topic, topic_len) != 0)
                atomic_store(&broker->failed, 1);
            else
                atomic_store(&broker->saw_publish, 1);
        }
    }
    close(fd);
    return NULL;
}

static void forward_broker_init(struct forward_broker *broker, int fd,
                                const char *expected_topic) {
    broker->broker_fd = fd;
    broker->expected_topic = expected_topic;
    atomic_init(&broker->saw_publish, 0);
    atomic_init(&broker->failed, 0);
}

enum {
    BRIDGE_PUBACK_MATCHING = 0,
    BRIDGE_PUBACK_MISMATCHED,
    BRIDGE_PUBACK_MALFORMED
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
    if (broker->puback_mode == BRIDGE_PUBACK_MISMATCHED) {
        puback[3] = (uint8_t)(puback[3] == 0xFF ? 0 : puback[3] + 1);
    } else if (broker->puback_mode == BRIDGE_PUBACK_MALFORMED) {
        puback[1] = 0x01;
    }
    if (send(fd, puback, sizeof(puback), 0) != (ssize_t)sizeof(puback))
        atomic_store(&broker->failed, 1);
    close(fd);
    return NULL;
}

static void *bridge_qos2_broker(void *arg) {
    struct bridge_broker *broker = arg;
    int fd = accept(broker->broker_fd, NULL, NULL);
    if (fd < 0) { atomic_store(&broker->failed, 1); return NULL; }
    uint8_t frame[256];
    if (bridge_read_exact(fd, frame, 2) != 0 || frame[0] != 0x10 ||
        bridge_read_exact(fd, frame + 2, frame[1]) != 0) goto fail;
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    if (send(fd, connack, sizeof(connack), 0) != (ssize_t)sizeof(connack) ||
        bridge_read_exact(fd, frame, 2) != 0 || (frame[0] & 0x06) != 0x04 ||
        bridge_read_exact(fd, frame + 2, frame[1]) != 0) goto fail;
    size_t topic_len = ((size_t)frame[2] << 8) | frame[3];
    size_t pos = 4 + topic_len;
    if (pos + 1 >= (size_t)frame[1] + 2) goto fail;
    uint16_t pid = ((uint16_t)frame[pos] << 8) | frame[pos + 1];
    uint8_t pubrec[] = {0x50, 0x02, (uint8_t)(pid >> 8), (uint8_t)pid};
    if (send(fd, pubrec, sizeof(pubrec), 0) != (ssize_t)sizeof(pubrec) ||
        bridge_read_exact(fd, frame, sizeof(uint8_t) * 4) != 0 ||
        frame[0] != 0x62 || frame[1] != 0x02 ||
        frame[2] != (uint8_t)(pid >> 8) || frame[3] != (uint8_t)pid) goto fail;
    uint8_t pubcomp[] = {0x70, 0x02, (uint8_t)(pid >> 8), (uint8_t)pid};
    if (send(fd, pubcomp, sizeof(pubcomp), 0) != (ssize_t)sizeof(pubcomp)) goto fail;
    close(fd); return NULL;
fail:
    atomic_store(&broker->failed, 1); close(fd); return NULL;
}

TEST(mqtt_bridge, qos2_completes_pubrec_pubrel_pubcomp) {
    int port = 0, broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker = { .broker_fd = broker_fd };
    atomic_init(&broker.saw_qos1, 0);
    atomic_init(&broker.failed, 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_qos2_broker, &broker), 0);
    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("qos2-test");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_add_mapping(bridge, "orders.qos2", "orders/qos2", 2), 0);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "orders.qos2", (const uint8_t *)"ok", 2), 1);
    pthread_join(thread, NULL);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    close(broker_fd);
}

TEST(mqtt_bridge, qos1_waits_for_matching_puback) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker;
    broker.broker_fd = broker_fd;
    broker.puback_mode = BRIDGE_PUBACK_MATCHING;
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

TEST(mqtt_bridge, unmapped_subject_is_off_by_default) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct forward_broker broker;
    forward_broker_init(&broker, broker_fd, NULL);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_forward_broker, &broker), 0);
    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("forward-off");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "unmapped.subject",
                                      (const uint8_t *)"x", 1), 0);
    pthread_join(thread, NULL);
    ASSERT_EQ(atomic_load(&broker.saw_publish), 0);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    close(broker_fd);
}

TEST(mqtt_bridge, forward_all_converts_unmapped_subject) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct forward_broker broker;
    forward_broker_init(&broker, broker_fd, "unmapped/subject");
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_forward_broker, &broker), 0);
    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("forward-on");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_bridge_set_forward_all(bridge, 1), 0);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "unmapped.subject",
                                      (const uint8_t *)"x", 1), 1);
    pthread_join(thread, NULL);
    ASSERT_EQ(atomic_load(&broker.saw_publish), 1);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    close(broker_fd);
}

TEST(mqtt_bridge, qos1_rejects_mismatched_puback) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker;
    broker.broker_fd = broker_fd;
    broker.puback_mode = BRIDGE_PUBACK_MISMATCHED;
    atomic_init(&broker.saw_qos1, 0);
    atomic_init(&broker.failed, 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_qos1_broker, &broker), 0);

    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("qos1-mismatch");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(bridge, "orders.created", "orders/created", 1), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "orders.created",
                                      (const uint8_t *)"bad", 3), -1);
    ASSERT_EQ(cmq_mqtt_bridge_is_connected(bridge), 0);
    ASSERT_EQ(atomic_load(&broker.saw_qos1), 1);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    pthread_join(thread, NULL);
    close(broker_fd);
}

TEST(mqtt_bridge, qos1_rejects_malformed_puback) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker;
    broker.broker_fd = broker_fd;
    broker.puback_mode = BRIDGE_PUBACK_MALFORMED;
    atomic_init(&broker.saw_qos1, 0);
    atomic_init(&broker.failed, 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_qos1_broker, &broker), 0);

    cmq_mqtt_bridge_t *bridge = cmq_mqtt_bridge_create("qos1-malformed");
    ASSERT_NOT_NULL(bridge);
    ASSERT_EQ(cmq_mqtt_bridge_connect(bridge, "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(bridge, "orders.created", "orders/created", 1), 0);
    ASSERT_EQ(cmq_mqtt_bridge_publish(bridge, "orders.created",
                                      (const uint8_t *)"bad", 3), -1);
    ASSERT_EQ(cmq_mqtt_bridge_is_connected(bridge), 0);
    ASSERT_EQ(atomic_load(&broker.saw_qos1), 1);
    ASSERT_EQ(atomic_load(&broker.failed), 0);
    cmq_mqtt_bridge_destroy(bridge);
    pthread_join(thread, NULL);
    close(broker_fd);
}

static void server_publish_reports_bridge_failure(int puback_mode) {
    int port = 0;
    int broker_fd = bridge_listen_loopback(&port);
    ASSERT(broker_fd >= 0);
    struct bridge_broker broker;
    broker.broker_fd = broker_fd;
    broker.puback_mode = puback_mode;
    atomic_init(&broker.saw_qos1, 0);
    atomic_init(&broker.failed, 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, bridge_qos1_broker, &broker), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25042 + puback_mode;
    cfg.log_to_stdout = 0;
    cfg.mqtt_bridge_addr = "127.0.0.1";
    cfg.mqtt_bridge_port = port;
    cfg.mqtt_bridge_maps[0].cmq_subject = strdup("orders.created");
    cfg.mqtt_bridge_maps[0].mqtt_topic = strdup("orders/created");
    cfg.mqtt_bridge_maps[0].qos = 1;
    cfg.mqtt_bridge_map_count = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* The local publish path still runs, but the upstream QoS1 failure is
     * part of the non-client publish result. */
    ASSERT_EQ(cmq_server_publish(srv, "orders.created",
                                  (const uint8_t *)"server", 6,
                                  "$default"), -1);
    ASSERT_EQ(cmq_atomic_load_u64(&srv->stat_publishes_rejected,
                                  CMQ_ATOMIC_RELAXED), 1);
    ASSERT_EQ(atomic_load(&broker.saw_qos1), 1);
    ASSERT_EQ(atomic_load(&broker.failed), 0);

    cmq_server_destroy(srv);
    pthread_join(thread, NULL);
    close(broker_fd);
}

TEST(mqtt_bridge, server_publish_reports_mismatched_puback) {
    server_publish_reports_bridge_failure(BRIDGE_PUBACK_MISMATCHED);
}

TEST(mqtt_bridge, server_publish_reports_malformed_puback) {
    server_publish_reports_bridge_failure(BRIDGE_PUBACK_MALFORMED);
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
