#include "cmq_account.h"
#include "cmq_tls.h"
#include "cmq_mqtt.h"
#include "cmq_ws.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

TEST(account, create_destroy) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_NOT_NULL(mgr);
    ASSERT_EQ(cmq_account_count(mgr), (size_t)0);
    cmq_account_manager_destroy(mgr);
}

TEST(account, create_delete) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "tenant-a"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "tenant-b"), 0);
    ASSERT_EQ(cmq_account_count(mgr), (size_t)2);

    ASSERT_EQ(cmq_account_create(mgr, "tenant-a"), 0);
    ASSERT_EQ(cmq_account_count(mgr), (size_t)2);

    cmq_account_t *a = cmq_account_get(mgr, "tenant-a", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_STR_EQ(a->name, "tenant-a");
    ASSERT_EQ(a->active, 1);
    cmq_account_release(mgr, a);

    ASSERT_EQ(cmq_account_delete(mgr, "tenant-a"), 0);
    ASSERT_EQ(cmq_account_count(mgr), (size_t)1);
    ASSERT_NULL(cmq_account_get(mgr, "tenant-a", NULL));

    /* Soft-deleted slot must not be reused for a different name while
       capacity remains (stable pointer for stale holders). */
    cmq_account_t *old = NULL;
    cmq_account_create(mgr, "keep-ptr");
    old = cmq_account_get(mgr, "keep-ptr", NULL);
    ASSERT_NOT_NULL(old);
    ASSERT_EQ(cmq_account_delete(mgr, "keep-ptr"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "other-name"), 0);
    ASSERT_STR_EQ(old->name, "keep-ptr"); /* pointer still names keep-ptr */
    ASSERT_EQ(old->active, 0);
    cmq_account_release(mgr, old);

    /* When the table is full, inactive slots are reclaimed for new names. */
    {
        cmq_account_manager_t *full = cmq_account_manager_create();
        char name[CMQ_ACCOUNT_NAME_SIZE];
        for (size_t i = 0; i < CMQ_ACCOUNT_MAX; i++) {
            snprintf(name, sizeof(name), "u%zu", i);
            ASSERT_EQ(cmq_account_create(full, name), 0);
        }
        ASSERT_EQ(cmq_account_create(full, "overflow"), -1);
        ASSERT_EQ(cmq_account_delete(full, "u0"), 0);
        ASSERT_EQ(cmq_account_create(full, "reclaimed"), 0);
        cmq_account_t *rc = cmq_account_get(full, "reclaimed", NULL);
        ASSERT_NOT_NULL(rc);
        cmq_account_release(full, rc);
        ASSERT_NULL(cmq_account_get(full, "u0", NULL));
        cmq_account_manager_destroy(full);
    }

    ASSERT_EQ(cmq_account_delete(mgr, "nonexistent"), -1);

    char too_long[CMQ_ACCOUNT_NAME_SIZE + 8];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    ASSERT_EQ(cmq_account_create(mgr, too_long), -1);
    ASSERT_NULL(cmq_account_get(mgr, too_long, NULL));

    cmq_account_manager_destroy(mgr);
}

TEST(account, stats) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "s1");
    cmq_account_t *a = cmq_account_get(mgr, "s1", NULL);
    ASSERT_NOT_NULL(a);

    cmq_account_inc_connections(a, a->epoch);
    cmq_account_inc_connections(a, a->epoch);
    ASSERT_EQ(a->connections, (uint64_t)2);

    cmq_account_dec_connections(a, a->epoch);
    ASSERT_EQ(a->connections, (uint64_t)1);

    cmq_account_inc_subscriptions(a, a->epoch);
    ASSERT_EQ(a->subscriptions, (uint64_t)1);

    cmq_account_inc_msgs_in(a, a->epoch, 100);
    cmq_account_inc_msgs_in(a, a->epoch, 200);
    ASSERT_EQ(a->messages_in, (uint64_t)2);
    ASSERT_EQ(a->bytes_in, (uint64_t)300);

    cmq_account_inc_msgs_out(a, a->epoch, 50);
    ASSERT_EQ(a->messages_out, (uint64_t)1);
    ASSERT_EQ(a->bytes_out, (uint64_t)50);
    cmq_account_release(mgr, a);

    cmq_account_manager_destroy(mgr);
}

TEST(account, exports_imports) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "acme");
    cmq_account_create(mgr, "globex");
    cmq_account_create(mgr, "other");

    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)1);

    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)1);

    ASSERT_EQ(cmq_account_add_import(mgr, "globex", "acme.>", "acme"), 0);
    ASSERT_EQ(cmq_account_import_count(mgr, "globex"), (size_t)1);

    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "acme.data"), 1);
    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "other.data"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "acme.data"), 1);
    /* Local / non-exported subjects stay allowed when imports exist. */
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "globex.orders"), 1);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "other.data"), 1);

    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "acme.>"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)0);

    /* Token wildcard * */
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.*.events", "globex"), 0);
    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "acme.prod.events"), 1);
    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "acme.prod.other"), 0);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "acme.*.events"), 0);

    /* Mid-pattern '>' is invalid — reject at add (NATS: '>' is final only). */
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>.secret", "globex"), -1);
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "foo.>bar", "globex"), -1);
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "foo.*bar", "globex"), -1);

    ASSERT_EQ(cmq_account_remove_import(mgr, "globex", "acme.>"), 0);
    ASSERT_EQ(cmq_account_import_count(mgr, "globex"), (size_t)0);

    /* Export without import → can_import denies (no ghost subscribe). */
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "acme.data"), 0);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "acme.>"), 0);

    /* Wildcard-only exports must not lock local SUBSCRIBE on other accounts. */
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", ">", "*"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "globex.orders"), 1);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", ">"), 0);
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "*.>", "*"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "globex.orders"), 1);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "*.>"), 0);
    /* Catch-all import must not claim every local subject either. */
    ASSERT_EQ(cmq_account_add_import(mgr, "globex", ">", "*"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "globex.orders"), 1);
    ASSERT_EQ(cmq_account_remove_import(mgr, "globex", ">"), 0);

    /* Import without export → can_import denies (align with may_deliver). */
    ASSERT_EQ(cmq_account_add_import(mgr, "globex", "acme.>", "acme"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "acme.data"), 0);
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "acme.data"), 1);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "acme.>"), 0);
    ASSERT_EQ(cmq_account_remove_import(mgr, "globex", "acme.>"), 0);

    /* remove_export deletes all dests for the same subject. */
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "shared.>", "globex"), 0);
    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "shared.>", "other"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)2);
    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "shared.>"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)0);

    cmq_account_manager_destroy(mgr);
}

TEST(account, may_deliver_cross_account) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "acme");
    cmq_account_create(mgr, "globex");
    cmq_account_create(mgr, "other");

    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "acme", "acme.data"), 1);
    /* No export/import → deny cross-account (not open-default). */
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "globex", "acme.data"), 0);

    cmq_account_add_export(mgr, "acme", "acme.>", "globex");
    cmq_account_add_import(mgr, "globex", "acme.>", "acme");
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "globex", "acme.data"), 1);
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "other", "acme.data"), 0);
    /* Same-account inbox replies must not be blocked by export allow-list. */
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "acme", "_INBOX.rr1"), 1);
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "acme", "other.data"), 0);

    /* Export without matching import must deny (not open-default). */
    cmq_account_remove_import(mgr, "globex", "acme.>");
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "globex", "acme.data"), 0);

    /* Soft-delete must purge sibling reverse ACL or can_import traps SUBSCRIBE. */
    ASSERT_EQ(cmq_account_add_import(mgr, "globex", "shared.>", "acme"), 0);
    ASSERT_EQ(cmq_account_add_export(mgr, "globex", "offered.>", "acme"), 0);
    ASSERT_EQ(cmq_account_delete(mgr, "acme"), 0);
    ASSERT_EQ(cmq_account_may_deliver(mgr, "acme", "globex", "acme.data"), 0);
    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "acme.data"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "shared.orders"), 1);
    ASSERT_EQ(cmq_account_import_count(mgr, "globex"), (size_t)0);
    ASSERT_EQ(cmq_account_export_count(mgr, "globex"), (size_t)0);
    /* Reactivate: stale sibling export offer must not lock local SUBSCRIBE. */
    ASSERT_EQ(cmq_account_create(mgr, "acme"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "acme", "offered.x"), 1);

    /* Reactivate bumps epoch so callers can detect stale sessions. */
    cmq_account_t *before = NULL;
    {
        cmq_account_manager_t *m2 = cmq_account_manager_create();
        ASSERT_EQ(cmq_account_create(m2, "ep"), 0);
        cmq_account_t *a = cmq_account_get(m2, "ep", NULL);
        ASSERT_NOT_NULL(a);
        uint32_t e0 = a->epoch;
        ASSERT_EQ(cmq_account_delete(m2, "ep"), 0);
        ASSERT_EQ(cmq_account_get(m2, "ep", NULL), NULL); /* inactive */
        /* Soft-delete itself bumps epoch so stale sessions cannot linger. */
        ASSERT(a->epoch != e0);
        e0 = a->epoch;
        cmq_account_release(m2, a);
        /* CONNECT path must refuse soft-deleted names. */
        ASSERT_EQ(cmq_account_ensure(m2, "ep"), -1);
        ASSERT_EQ(cmq_account_create(m2, "ep"), 0);
        a = cmq_account_get(m2, "ep", NULL);
        ASSERT_NOT_NULL(a);
        ASSERT(a->epoch != e0);
        ASSERT_EQ(cmq_account_ensure(m2, "ep"), 0); /* already active */
        ASSERT_EQ(cmq_account_ensure(m2, "brand-new"), 0);
        cmq_account_release(m2, a);
        (void)before;
        cmq_account_manager_destroy(m2);
    }

    cmq_account_manager_destroy(mgr);
}

TEST(account, perms_isolated_per_manager) {
    cmq_account_manager_t *a = cmq_account_manager_create();
    cmq_account_manager_t *b = cmq_account_manager_create();
    cmq_account_create(a, "acme");
    cmq_account_create(a, "globex");
    cmq_account_create(b, "acme");
    cmq_account_create(b, "globex");

    ASSERT_EQ(cmq_account_add_export(a, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_add_import(a, "globex", "acme.>", "acme"), 0);
    ASSERT_EQ(cmq_account_may_deliver(a, "acme", "globex", "acme.data"), 1);
    /* Manager B must not inherit A's ACL. */
    ASSERT_EQ(cmq_account_may_deliver(b, "acme", "globex", "acme.data"), 0);
    ASSERT_EQ(cmq_account_export_count(b, "acme"), (size_t)0);

    cmq_account_manager_destroy(a);
    cmq_account_manager_destroy(b);
}

TEST(account, reject_overlong_acl_names) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "victim");
    char long_name[CMQ_ACCOUNT_NAME_SIZE + 8];
    memset(long_name, 'v', CMQ_ACCOUNT_NAME_SIZE - 1);
    memcpy(long_name, "victim", 6);
    long_name[CMQ_ACCOUNT_NAME_SIZE - 1] = 'X';
    long_name[CMQ_ACCOUNT_NAME_SIZE] = '\0';
    /* Must not truncate into victim's ACL bucket. */
    ASSERT_EQ(cmq_account_add_export(mgr, long_name, "x.>", "victim"), -1);
    ASSERT_EQ(cmq_account_export_count(mgr, "victim"), (size_t)0);
    ASSERT_EQ(cmq_account_add_export(mgr, "victim", "x.>", long_name), -1);
    cmq_account_manager_destroy(mgr);
}

TEST(tls, config_create_destroy) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_configured(cfg), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(tls, config_set_fields) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_cert(cfg, "/path/to/cert.pem"), 0);
    ASSERT_EQ(cmq_tls_set_key(cfg, "/path/to/key.pem"), 0);
    ASSERT_EQ(cmq_tls_set_ca(cfg, "/path/to/ca.pem"), 0);
    ASSERT_EQ(cmq_tls_set_verify(cfg, 1), 0);
    ASSERT_EQ(cmq_tls_set_server_name(cfg, "example.com"), 0);

    ASSERT_STR_EQ(cmq_tls_cert_path(cfg), "/path/to/cert.pem");
    ASSERT_STR_EQ(cmq_tls_key_path(cfg), "/path/to/key.pem");
    ASSERT_STR_EQ(cmq_tls_ca_path(cfg), "/path/to/ca.pem");
    ASSERT_EQ(cmq_tls_verify_peer(cfg), 1);
    ASSERT_STR_EQ(cmq_tls_server_name(cfg), "example.com");
    ASSERT_EQ(cmq_tls_configured(cfg), 1);
    ASSERT_EQ(cmq_tls_backend_secure(), 0);

    cmq_tls_config_destroy(cfg);
}

TEST(tls, session_lifecycle) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    cmq_tls_set_cert(cfg, "c");
    cmq_tls_set_key(cfg, "k");

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    cmq_tls_session_t *srv = cmq_tls_server_session(cfg, pipefd[0]);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(cmq_tls_fd(srv), pipefd[0]);
    ASSERT_EQ(cmq_tls_handshake(srv), 0);

    const char *msg = "hello tls";
    ssize_t w = write(pipefd[1], msg, strlen(msg));
    ASSERT(w > 0);

    uint8_t buf[64];
    ssize_t r = cmq_tls_read(srv, buf, sizeof(buf));
    ASSERT(r > 0);
    ASSERT_EQ((size_t)r, strlen(msg));
    ASSERT(memcmp(buf, msg, strlen(msg)) == 0);

    cmq_tls_session_destroy(srv);
    close(pipefd[1]);
    cmq_tls_config_destroy(cfg);
}

TEST(mqtt, bridge_create_destroy) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("test-client");
    ASSERT_NOT_NULL(br);
    char id[CMQ_MQTT_CLIENT_ID];
    ASSERT_EQ(cmq_mqtt_client_id(br, id, sizeof(id)), 0);
    ASSERT_STR_EQ(id, "test-client");
    ASSERT_EQ(cmq_mqtt_bridge_is_connected(br), 0);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqtt, mapping) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("m1");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.temp", "sensor/temp", 1), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.humidity", "sensor/humidity", 0), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)2);

    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.temp", "sensor/temp", 2), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "bad", "bad/qos", 3), -1);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)2);

    char too_long[CMQ_MQTT_TOPIC_MAX + 8];
    memset(too_long, 't', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "ok", too_long, 0), -1);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, too_long, "ok/topic", 0), -1);

    cmq_mqtt_mapping_t m;
    ASSERT_EQ(cmq_mqtt_find_mapping(br, "sensor/temp", &m), 0);
    ASSERT_STR_EQ(m.cmq_subject, "sensor.temp");
    ASSERT_EQ(m.qos, 2);

    ASSERT_EQ(cmq_mqtt_remove_mapping(br, "sensor/temp"), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)1);
    ASSERT_EQ(cmq_mqtt_find_mapping(br, "sensor/temp", &m), -1);

    ASSERT_EQ(cmq_mqtt_remove_mapping(br, "nonexistent"), -1);

    cmq_mqtt_bridge_destroy(br);
}

TEST(mqtt, topic_conversion) {
    char buf[256];
    const char *sub = cmq_mqtt_topic_to_subject("sensor/temperature/room1", buf, sizeof(buf));
    ASSERT_NOT_NULL(sub);
    ASSERT_STR_EQ(sub, "sensor.temperature.room1");

    const char *top = cmq_mqtt_subject_to_topic("sensor.temperature.room1", buf, sizeof(buf));
    ASSERT_NOT_NULL(top);
    ASSERT_STR_EQ(top, "sensor/temperature/room1");

    /* MQTT + and # map to NATS * and > */
    sub = cmq_mqtt_topic_to_subject("a/+/b/#", buf, sizeof(buf));
    ASSERT_NOT_NULL(sub);
    ASSERT_STR_EQ(sub, "a.*.b.>");
    top = cmq_mqtt_subject_to_topic("a.*.b.>", buf, sizeof(buf));
    ASSERT_NOT_NULL(top);
    ASSERT_STR_EQ(top, "a/+/b/#");

    /* Undersized buf must fail closed — no silent truncation. */
    char tiny[8];
    ASSERT_NULL(cmq_mqtt_topic_to_subject("sensor/temperature/room1", tiny, sizeof(tiny)));
    ASSERT_NULL(cmq_mqtt_subject_to_topic("sensor.temperature.room1", tiny, sizeof(tiny)));
}

TEST(mqtt, encode_connect) {
    uint8_t buf[256];
    int len = cmq_mqtt_encode_connect(buf, sizeof(buf), "client1", 60, 1);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 1);
}

TEST(mqtt, decode_connack) {
    uint8_t ok[] = {0x20, 0x02, 0x00, 0x00};
    ASSERT_EQ(cmq_mqtt_decode_connack(ok, sizeof(ok)), 0);
    uint8_t bad_rl[] = {0x20, 0x03, 0x00, 0x00};
    ASSERT_EQ(cmq_mqtt_decode_connack(bad_rl, sizeof(bad_rl)), -1);
    uint8_t refused[] = {0x20, 0x02, 0x00, 0x05};
    ASSERT_EQ(cmq_mqtt_decode_connack(refused, sizeof(refused)), 5);
    /* Trailing pipelined bytes must not decode as success. */
    uint8_t trail[] = {0x20, 0x02, 0x00, 0x00, 0x30};
    ASSERT_EQ(cmq_mqtt_decode_connack(trail, sizeof(trail)), -1);
}

TEST(mqtt, encode_publish) {
    uint8_t buf[256];
    const uint8_t *payload = (const uint8_t *)"hello";
    int len = cmq_mqtt_encode_publish(buf, sizeof(buf), "test/topic",
                                       payload, 5, 0, 0);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 3);
    len = cmq_mqtt_encode_publish(buf, sizeof(buf), "empty/topic", NULL, 0, 0, 0);
    ASSERT(len > 0);
    len = cmq_mqtt_encode_publish(buf, sizeof(buf), "t", payload, 5, 1, 7);
    ASSERT(len > 0);
    ASSERT_EQ(buf[len - 7], 0x00); /* packet id hi before payload "hello" */
    ASSERT_EQ(buf[len - 6], 0x07);
    ASSERT_EQ(cmq_mqtt_encode_publish(buf, sizeof(buf), "t", payload, 5, 1, 0), -1);
    ASSERT_EQ(cmq_mqtt_encode_publish(buf, sizeof(buf), "t", payload, 5, 3, 1), -1);
    ASSERT_EQ(cmq_mqtt_encode_publish(buf, sizeof(buf), "t", payload, 5, -1, 1), -1);
}

TEST(mqtt, encode_subscribe) {
    uint8_t buf[256];
    int len = cmq_mqtt_encode_subscribe(buf, sizeof(buf), "test/topic", 1, 9);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 8);
    ASSERT_EQ(cmq_mqtt_encode_subscribe(buf, sizeof(buf), "test/topic", 1, 0), -1);
    ASSERT_EQ(cmq_mqtt_encode_subscribe(buf, sizeof(buf), "test/topic", 3, 1), -1);
}

TEST(mqtt, encode_pingreq) {
    uint8_t buf[16];
    int len = cmq_mqtt_encode_pingreq(buf, sizeof(buf));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 12);
}

TEST(mqtt, bridge_info) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("info-test");
    cmq_mqtt_bridge_info_t info = cmq_mqtt_bridge_info(br);
    ASSERT_STR_EQ(info.client_id, "info-test");
    ASSERT_EQ(info.connected, 0);
    ASSERT_EQ(info.keepalive_ms, 60000);
    ASSERT_EQ(info.clean_session, 1);
    cmq_mqtt_bridge_destroy(br);
}

TEST(ws, server_create_destroy) {
    cmq_ws_server_t *srv = cmq_ws_server_create(8080);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(cmq_ws_server_client_count(srv), (size_t)0);
    cmq_ws_server_destroy(srv);
}

TEST(ws, frame_parse_small) {
    uint8_t raw[] = {0x81, 0x05, 0x48, 0x65, 0x6C, 0x6C, 0x6F};
    cmq_ws_frame_t frame = {0};
    int r = cmq_ws_frame_parse(raw, sizeof(raw), &frame);
    ASSERT(r > 0);
    ASSERT_EQ(frame.fin, 1);
    ASSERT_EQ(frame.opcode, CMQ_WS_OPCODE_TEXT);
    ASSERT_EQ(frame.payload_len, (size_t)5);
    ASSERT(memcmp(frame.payload, "Hello", 5) == 0);
}

TEST(ws, frame_parse_masked) {
    uint8_t raw[] = {0x82, 0x85, 0x37, 0xFA, 0x21, 0x3D, 0x7F, 0x9F, 0x4D, 0x51, 0x58};
    cmq_ws_frame_t frame = {0};
    int r = cmq_ws_frame_parse(raw, sizeof(raw), &frame);
    ASSERT(r > 0);
    ASSERT_EQ(frame.opcode, CMQ_WS_OPCODE_BINARY);
    ASSERT_EQ(frame.masked, 1);
    ASSERT_EQ(frame.mask_key, (uint32_t)0x37FA213D);

    uint8_t unmasked[5];
    memcpy(unmasked, frame.payload, 5);
    cmq_ws_mask(unmasked, 5, frame.mask_key);
    cmq_ws_mask(unmasked, 5, frame.mask_key);
    ASSERT(memcmp(unmasked, frame.payload, 5) == 0);
}

TEST(ws, frame_serialize) {
    const uint8_t payload[] = "test";
    cmq_ws_frame_t frame = {0};
    frame.fin = 1;
    frame.opcode = CMQ_WS_OPCODE_BINARY;
    frame.payload = (uint8_t *)payload;
    frame.payload_len = 4;

    uint8_t buf[32];
    int len = cmq_ws_frame_serialize(&frame, buf, sizeof(buf));
    ASSERT_EQ(len, 6);
    ASSERT_EQ(buf[0], 0x82);
    ASSERT_EQ(buf[1], 0x04);
    ASSERT(memcmp(&buf[2], "test", 4) == 0);

    /* Overflow: payload_len near SIZE_MAX must not wrap past buf_len. */
    frame.payload_len = SIZE_MAX - 1;
    frame.payload = NULL;
    ASSERT_EQ(cmq_ws_frame_serialize(&frame, buf, sizeof(buf)), -1);

    frame.payload_len = 4;
    frame.payload = NULL;
    ASSERT_EQ(cmq_ws_frame_serialize(&frame, buf, sizeof(buf)), -1);
}

TEST(ws, mask_unmask) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t orig[5];
    memcpy(orig, data, 5);
    cmq_ws_mask(data, 5, 0x12345678);
    ASSERT(memcmp(data, orig, 5) != 0);
    cmq_ws_mask(data, 5, 0x12345678);
    ASSERT(memcmp(data, orig, 5) == 0);
}

TEST(ws, parse_http_upgrade) {
    const char *req = "GET /ws HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Upgrade: websocket\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "\r\n";
    char key[64];
    int r = cmq_ws_parse_http_upgrade(req, strlen(req), key, sizeof(key));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(key, "dGhlIHNhbXBsZSBub25jZQ==");

    /* Non-NUL-terminated buffer with exact req_len must still parse. */
    char raw[128];
    size_t n = strlen(req);
    memcpy(raw, req, n);
    memset(key, 0xAA, sizeof(key));
    ASSERT_EQ(cmq_ws_parse_http_upgrade(raw, n, key, sizeof(key)), 0);
    ASSERT_STR_EQ(key, "dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_EQ(cmq_ws_parse_http_upgrade(req, n, key, 0), -1);

    /* Header names are case-insensitive. */
    const char *req_lc = "GET /ws HTTP/1.1\r\n"
                          "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "\r\n";
    ASSERT_EQ(cmq_ws_parse_http_upgrade(req_lc, strlen(req_lc), key, sizeof(key)), 0);
    ASSERT_STR_EQ(key, "dGhlIHNhbXBsZSBub25jZQ==");
}

TEST(ws, build_response) {
    char resp[512];
    int r = cmq_ws_build_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", resp, sizeof(resp));
    ASSERT_EQ(r, 0);
    ASSERT(strstr(resp, "101 Switching Protocols") != NULL);
    ASSERT(strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    /* Truncation / header injection must fail closed. */
    ASSERT_EQ(cmq_ws_build_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", resp, 32), -1);
    ASSERT_EQ(cmq_ws_build_response("bad\r\nX-Injected: 1", resp, sizeof(resp)), -1);
}

TEST(ws, accept_key_bounds) {
    char out[64];
    ASSERT_EQ(cmq_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    ASSERT_EQ(cmq_ws_accept_key("", out, sizeof(out)), -1);
    char huge[160];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    ASSERT_EQ(cmq_ws_accept_key(huge, out, sizeof(out)), -1);
}

TEST(ws, parse_medium_frame) {
    uint8_t hdr[4] = {0x82, 0x7E, 0x00, 0x80};
    uint8_t buf[132];
    memcpy(buf, hdr, 4);
    memset(&buf[4], 'B', 128);

    cmq_ws_frame_t frame = {0};
    int r = cmq_ws_frame_parse(buf, sizeof(buf), &frame);
    ASSERT(r > 0);
    ASSERT_EQ(frame.payload_len, (size_t)128);
    ASSERT_EQ(frame.opcode, CMQ_WS_OPCODE_BINARY);
}

TEST(ws, parse_need_more_vs_fatal) {
    uint8_t partial[] = {0x82, 0x05}; /* header only, need payload */
    cmq_ws_frame_t frame = {0};
    ASSERT_EQ(cmq_ws_frame_parse(partial, sizeof(partial), &frame), 0);

    /* 127-length with MSB set — fatal */
    uint8_t bad[10] = {0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_EQ(cmq_ws_frame_parse(bad, sizeof(bad), &frame), -1);
}

TEST(ws, reject_rsv_and_fragmented_control) {
    cmq_ws_frame_t frame = {0};
    uint8_t rsv[] = {0xC2, 0x00}; /* FIN + RSV1 + BINARY */
    ASSERT_EQ(cmq_ws_frame_parse(rsv, sizeof(rsv), &frame), -1);
    uint8_t frag_ping[] = {0x09, 0x00}; /* PING without FIN */
    ASSERT_EQ(cmq_ws_frame_parse(frag_ping, sizeof(frag_ping), &frame), -1);
}

TEST(ws, reject_oversized_control) {
    cmq_ws_frame_t frame = {0};
    /* CLOSE with 126 extended length — control frames must use 7-bit ≤125. */
    uint8_t close126[] = {0x88, 0x7E, 0x00, 0x80};
    ASSERT_EQ(cmq_ws_frame_parse(close126, sizeof(close126), &frame), -1);
    /* PING claiming 126 via 7-bit field alone (invalid encoding path). */
    uint8_t ping_big[] = {0x89, 0x7E, 0x00, 0x01, 0x00};
    ASSERT_EQ(cmq_ws_frame_parse(ping_big, sizeof(ping_big), &frame), -1);
}

TEST_MAIN()
