#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_test.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
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
        int rc = cmq_parser_feed(parser, buf, (size_t)n);
        if (rc != 1) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
            continue;
        }
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (!f) return -1;
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

TEST(server, create_destroy) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    ASSERT_NOT_NULL(srv);
    cmq_server_destroy(srv);
}

TEST(server, bind_accept_info) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18801;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(18801);
    ASSERT(fd >= 0);

    nanosleep(&ts, NULL);

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server, connect_pong) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18802;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(18802);
    ASSERT(fd >= 0);

    struct timespec ts2 = {0, 100000000};
    nanosleep(&ts2, NULL);

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t frame;

    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    free_frame_payload(&frame);

    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    ts.tv_sec = 0; ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);

    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    free_frame_payload(&frame);

    send_frame(fd, CMQ_OP_PING, NULL, 0);
    ts.tv_sec = 0; ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);

    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_PONG);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server, pubsub_basic) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18803;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int sub_fd = connect_to(18803);
    ASSERT(sub_fd >= 0);
    cmq_parser_t *sub_parser = cmq_parser_create();
    cmq_frame_t frame;

    struct timespec ts3 = {0, 100000000};
    nanosleep(&ts3, NULL);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    free_frame_payload(&frame);

    send_frame(sub_fd, CMQ_OP_CONNECT, NULL, 0);
    ts.tv_sec = 0; ts.tv_nsec = 50000000;
    nanosleep(&ts, NULL);
    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    free_frame_payload(&frame);

    const char *subject = "test.topic";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    uint32_t sub_id = 42;
    sub_pl[0] = (sub_id >> 24) & 0xFF;
    sub_pl[1] = (sub_id >> 16) & 0xFF;
    sub_pl[2] = (sub_id >> 8) & 0xFF;
    sub_pl[3] = sub_id & 0xFF;
    sub_pl[4] = (slen >> 8) & 0xFF;
    sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);

    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    ts.tv_sec = 0; ts.tv_nsec = 50000000;
    nanosleep(&ts, NULL);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_SUBACK);
    ASSERT_EQ(frame.payload_len, (size_t)5);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);

    int pub_fd = connect_to(18803);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pub_parser = cmq_parser_create();

    struct timespec ts4 = {0, 100000000};
    nanosleep(&ts4, NULL);

    ASSERT_EQ(recv_frame(pub_fd, &frame, pub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    free_frame_payload(&frame);

    send_frame(pub_fd, CMQ_OP_CONNECT, NULL, 0);
    ts.tv_sec = 0; ts.tv_nsec = 50000000;
    nanosleep(&ts, NULL);
    ASSERT_EQ(recv_frame(pub_fd, &frame, pub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    free_frame_payload(&frame);

    const char *msg = "hello";
    size_t msg_len = strlen(msg);
    uint8_t pub_pl[256];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0;
    pub_pl[off++] = 0;
    memcpy(pub_pl + off, msg, msg_len);
    off += msg_len;

    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
    ts.tv_sec = 0; ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_MESSAGE);
    free_frame_payload(&frame);

    cmq_parser_destroy(pub_parser);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
