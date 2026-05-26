#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_account.h"
#include "cmq_ws.h"
#include "cmq_test.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    return fd;
}

static ssize_t send_frame(int fd, cmq_op_t op, const uint8_t *payload, size_t plen) {
    uint8_t buf[4096];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
    if (len == 0) return -1;
    return write(fd, buf, len);
}

static int recv_frame(int fd, cmq_frame_t *frame, cmq_parser_t *parser) {
    for (int retry = 0; retry < 50; retry++) {
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (f) {
            frame->hdr = f->hdr;
            frame->payload_len = f->payload_len;
            if (f->payload_len > 0 && f->payload) {
                frame->payload = malloc(f->payload_len);
                memcpy(frame->payload, f->payload, f->payload_len);
            } else {
                frame->payload = NULL;
            }
            cmq_parser_next(parser);
            return 0;
        }
        uint8_t buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        cmq_parser_feed(parser, buf, (size_t)n);
    }
    return -1;
}

static void free_frame_payload(cmq_frame_t *frame) {
    free(frame->payload);
    frame->payload = NULL;
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);
}

TEST(integration, account_stats_on_connect) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19101;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    ASSERT_NOT_NULL(srv->accounts);

    cmq_account_t *def_acc = cmq_account_get(srv->accounts, "$default");
    ASSERT_NOT_NULL(def_acc);
    ASSERT_EQ(def_acc->connections, (uint64_t)0);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(19101);
    ASSERT(fd >= 0);
    wait_ms(50);

    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    cmq_account_t *acc_after = cmq_account_get(srv->accounts, "$default");
    ASSERT_NOT_NULL(acc_after);
    ASSERT(acc_after->connections >= 1);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(integration, account_stats_on_pubsub) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19102;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int sub_fd = connect_to(19102);
    ASSERT(sub_fd >= 0);
    wait_ms(50);

    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);

    uint8_t sub_payload[256];
    char sub_subject[] = "stats.test";
    uint32_t sub_id = 1;
    sub_payload[0] = (sub_id >> 24) & 0xFF;
    sub_payload[1] = (sub_id >> 16) & 0xFF;
    sub_payload[2] = (sub_id >> 8) & 0xFF;
    sub_payload[3] = sub_id & 0xFF;
    sub_payload[4] = 0;
    sub_payload[5] = (uint8_t)strlen(sub_subject);
    memcpy(sub_payload + 6, sub_subject, strlen(sub_subject));
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_payload, 6 + strlen(sub_subject));
    wait_ms(50);

    cmq_frame_t suback;
    recv_frame(sub_fd, &suback, sub_parser);
    ASSERT_EQ(suback.hdr.op, CMQ_OP_SUBACK);
    free_frame_payload(&suback);

    int pub_fd = connect_to(19102);
    ASSERT(pub_fd >= 0);
    wait_ms(50);

    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);

    uint8_t pub_payload[256];
    char pub_subject[] = "stats.test";
    uint16_t slen = (uint16_t)strlen(pub_subject);
    pub_payload[0] = (slen >> 8) & 0xFF;
    pub_payload[1] = slen & 0xFF;
    memcpy(pub_payload + 2, pub_subject, slen);
    size_t off = 2 + slen;
    pub_payload[off++] = 0; /* reply_len high */
    pub_payload[off++] = 0; /* reply_len low */
    const char *msg = "hello stats";
    memcpy(pub_payload + off, msg, strlen(msg));
    off += strlen(msg);
    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_payload, off);
    wait_ms(200);

    cmq_account_t *acc = cmq_account_get(srv->accounts, "$default");
    ASSERT_NOT_NULL(acc);
    ASSERT(acc->messages_in >= 1);

    cmq_frame_t msg_frame;
    ASSERT_EQ(recv_frame(sub_fd, &msg_frame, sub_parser), 0);
    ASSERT_EQ(msg_frame.hdr.op, CMQ_OP_MESSAGE);
    free_frame_payload(&msg_frame);

    acc = cmq_account_get(srv->accounts, "$default");
    ASSERT(acc->messages_out >= 1);

    cmq_parser_destroy(pub_parser);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(integration, ws_upgrade_detection) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19103;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19103);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

    const char *upgrade_req = "GET /ws HTTP/1.1\r\n"
                               "Host: localhost:19103\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Sec-WebSocket-Version: 13\r\n\r\n";
    ssize_t w = write(fd, upgrade_req, strlen(upgrade_req));
    ASSERT(w > 0);
    wait_ms(200);

    char response[1024] = {0};
    ssize_t r = read(fd, response, sizeof(response) - 1);
    ASSERT(r > 0);
    response[r] = '\0';
    ASSERT(strstr(response, "101 Switching Protocols") != NULL);
    ASSERT(strstr(response, "Upgrade: websocket") != NULL);
    ASSERT(strstr(response, "Sec-WebSocket-Accept:") != NULL);

    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(integration, account_create_delete_via_api) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19104;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    ASSERT_EQ(cmq_account_create(srv->accounts, "tenant-acme"), 0);
    ASSERT_EQ(cmq_account_create(srv->accounts, "tenant-globex"), 0);
    ASSERT_EQ(cmq_account_count(srv->accounts), (size_t)3);

    cmq_account_t *acme = cmq_account_get(srv->accounts, "tenant-acme");
    ASSERT_NOT_NULL(acme);
    ASSERT_EQ(acme->active, 1);

    ASSERT_EQ(cmq_account_delete(srv->accounts, "tenant-acme"), 0);
    ASSERT_EQ(cmq_account_count(srv->accounts), (size_t)2);
    ASSERT_NULL(cmq_account_get(srv->accounts, "tenant-acme"));

    cmq_server_destroy(srv);
}

TEST_MAIN()
