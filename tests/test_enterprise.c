#include "cmq_account.h"
#include "cmq_tls.h"
#include "cmq_mqtt.h"
#include "cmq_ws.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

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

    cmq_account_t *a = cmq_account_get(mgr, "tenant-a");
    ASSERT_NOT_NULL(a);
    ASSERT_STR_EQ(a->name, "tenant-a");
    ASSERT_EQ(a->active, 1);

    ASSERT_EQ(cmq_account_delete(mgr, "tenant-a"), 0);
    ASSERT_EQ(cmq_account_count(mgr), (size_t)1);
    ASSERT_NULL(cmq_account_get(mgr, "tenant-a"));

    ASSERT_EQ(cmq_account_delete(mgr, "nonexistent"), -1);

    cmq_account_manager_destroy(mgr);
}

TEST(account, stats) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "s1");
    cmq_account_t *a = cmq_account_get(mgr, "s1");

    cmq_account_inc_connections(a);
    cmq_account_inc_connections(a);
    ASSERT_EQ(a->connections, (uint64_t)2);

    cmq_account_dec_connections(a);
    ASSERT_EQ(a->connections, (uint64_t)1);

    cmq_account_inc_subscriptions(a);
    ASSERT_EQ(a->subscriptions, (uint64_t)1);

    cmq_account_inc_msgs_in(a, 100);
    cmq_account_inc_msgs_in(a, 200);
    ASSERT_EQ(a->messages_in, (uint64_t)2);
    ASSERT_EQ(a->bytes_in, (uint64_t)300);

    cmq_account_inc_msgs_out(a, 50);
    ASSERT_EQ(a->messages_out, (uint64_t)1);
    ASSERT_EQ(a->bytes_out, (uint64_t)50);

    cmq_account_manager_destroy(mgr);
}

TEST(account, exports_imports) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_create(mgr, "acme");
    cmq_account_create(mgr, "globex");

    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)1);

    ASSERT_EQ(cmq_account_add_export(mgr, "acme", "acme.>", "globex"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)1);

    ASSERT_EQ(cmq_account_add_import(mgr, "globex", "acme.>", "acme"), 0);
    ASSERT_EQ(cmq_account_import_count(mgr, "globex"), (size_t)1);

    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "acme.data"), 1);
    ASSERT_EQ(cmq_account_can_export(mgr, "acme", "other.data"), 0);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "acme.data"), 1);
    ASSERT_EQ(cmq_account_can_import(mgr, "globex", "other.data"), 0);

    ASSERT_EQ(cmq_account_remove_export(mgr, "acme", "acme.>"), 0);
    ASSERT_EQ(cmq_account_export_count(mgr, "acme"), (size_t)0);

    ASSERT_EQ(cmq_account_remove_import(mgr, "globex", "acme.>"), 0);
    ASSERT_EQ(cmq_account_import_count(mgr, "globex"), (size_t)0);

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
    ASSERT_STR_EQ(cmq_mqtt_client_id(br), "test-client");
    ASSERT_EQ(cmq_mqtt_bridge_is_connected(br), 0);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqtt, mapping) {
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("m1");
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.temp", "sensor/temp", 1), 0);
    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.humidity", "sensor/humidity", 0), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)2);

    ASSERT_EQ(cmq_mqtt_add_mapping(br, "sensor.temp", "sensor/temp", 2), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)2);

    cmq_mqtt_mapping_t *m = cmq_mqtt_find_mapping(br, "sensor/temp");
    ASSERT_NOT_NULL(m);
    ASSERT_STR_EQ(m->cmq_subject, "sensor.temp");
    ASSERT_EQ(m->qos, 2);

    ASSERT_EQ(cmq_mqtt_remove_mapping(br, "sensor/temp"), 0);
    ASSERT_EQ(cmq_mqtt_mapping_count(br), (size_t)1);
    ASSERT_NULL(cmq_mqtt_find_mapping(br, "sensor/temp"));

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
}

TEST(mqtt, encode_connect) {
    uint8_t buf[256];
    int len = cmq_mqtt_encode_connect(buf, sizeof(buf), "client1", 60, 1);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 1);
}

TEST(mqtt, encode_publish) {
    uint8_t buf[256];
    const uint8_t *payload = (const uint8_t *)"hello";
    int len = cmq_mqtt_encode_publish(buf, sizeof(buf), "test/topic",
                                       payload, 5, 0);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 3);
}

TEST(mqtt, encode_subscribe) {
    uint8_t buf[256];
    int len = cmq_mqtt_encode_subscribe(buf, sizeof(buf), "test/topic", 1);
    ASSERT(len > 0);
    ASSERT_EQ(cmq_mqtt_decode_packet_type(buf, (size_t)len), 8);
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
}

TEST(ws, build_response) {
    char resp[512];
    int r = cmq_ws_build_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", resp, sizeof(resp));
    ASSERT_EQ(r, 0);
    ASSERT(strstr(resp, "101 Switching Protocols") != NULL);
    ASSERT(strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
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

TEST_MAIN()
