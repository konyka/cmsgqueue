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

static ssize_t send_frame_flags(int fd, cmq_op_t op, uint8_t flags,
                                  const uint8_t *payload, size_t plen) {
    uint8_t buf[8192];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, flags, payload, plen);
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

static void wait_server(void) {
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
}

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void do_connect(int fd, cmq_parser_t *parser) {
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);
}

TEST(phase2, auth_success) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18901;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18901);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    const char *user = "admin";
    const char *pass = "secret";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);

    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, auth_failure) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18902;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18902);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    const char *user = "admin";
    const char *pass = "wrong";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);

    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 2);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, queue_group_delivery) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18903;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int sub1 = connect_to(18903);
    int sub2 = connect_to(18903);
    ASSERT(sub1 >= 0);
    ASSERT(sub2 >= 0);
    wait_server();

    cmq_parser_t *p1 = cmq_parser_create();
    cmq_parser_t *p2 = cmq_parser_create();
    do_connect(sub1, p1);
    do_connect(sub2, p2);

    const char *subject = "work.tasks";
    uint16_t slen = (uint16_t)strlen(subject);
    const char *queue = "workers";
    uint16_t qlen = (uint16_t)strlen(queue);

    uint8_t sub_pl[128];
    sub_pl[0] = 0; sub_pl[1] = 0; sub_pl[2] = 0; sub_pl[3] = 1;
    sub_pl[4] = (slen >> 8) & 0xFF; sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    sub_pl[6 + slen] = (qlen >> 8) & 0xFF;
    sub_pl[6 + slen + 1] = qlen & 0xFF;
    memcpy(sub_pl + 6 + slen + 2, queue, qlen);
    send_frame(sub1, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen + 2 + qlen);
    wait_ms(50);

    cmq_frame_t frame;
    recv_frame(sub1, &frame, p1);
    free_frame_payload(&frame);

    sub_pl[3] = 2;
    send_frame(sub2, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen + 2 + qlen);
    wait_ms(50);
    recv_frame(sub2, &frame, p2);
    free_frame_payload(&frame);

    int pub_fd = connect_to(18903);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    uint8_t pub_pl[64];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0;
    pub_pl[off++] = 0;
    const char *msg = "task1";
    memcpy(pub_pl + off, msg, strlen(msg));
    off += strlen(msg);

    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
    wait_ms(100);

    int got_sub1 = (recv_frame(sub1, &frame, p1) == 0);
    if (got_sub1) free_frame_payload(&frame);
    int got_sub2 = (recv_frame(sub2, &frame, p2) == 0);
    if (got_sub2) free_frame_payload(&frame);

    ASSERT(got_sub1 + got_sub2 == 1);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(p1);
    cmq_parser_destroy(p2);
    close(pub_fd);
    close(sub1);
    close(sub2);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, headers_passthrough) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18904;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int sub_fd = connect_to(18904);
    ASSERT(sub_fd >= 0);
    wait_server();
    cmq_parser_t *sp = cmq_parser_create();
    do_connect(sub_fd, sp);

    const char *subject = "hdr.test";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    sub_pl[0] = 0; sub_pl[1] = 0; sub_pl[2] = 0; sub_pl[3] = 1;
    sub_pl[4] = (slen >> 8) & 0xFF; sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(sub_fd, &frame, sp);
    free_frame_payload(&frame);

    int pub_fd = connect_to(18904);
    ASSERT(pub_fd >= 0);
    wait_server();
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    const char *hdr_key = "X-Id";
    const char *hdr_val = "42";
    size_t hdr_total = 1 + strlen(hdr_key) + 1 + strlen(hdr_val);
    const char *msg = "hello";
    size_t msg_len = strlen(msg);

    uint8_t pub_pl[256];
    size_t poff = 0;
    pub_pl[poff++] = (slen >> 8) & 0xFF;
    pub_pl[poff++] = slen & 0xFF;
    memcpy(pub_pl + poff, subject, slen);
    poff += slen;
    pub_pl[poff++] = 0;
    pub_pl[poff++] = 0;
    pub_pl[poff++] = (hdr_total >> 8) & 0xFF;
    pub_pl[poff++] = hdr_total & 0xFF;
    pub_pl[poff++] = (uint8_t)strlen(hdr_key);
    memcpy(pub_pl + poff, hdr_key, strlen(hdr_key));
    poff += strlen(hdr_key);
    pub_pl[poff++] = (uint8_t)strlen(hdr_val);
    memcpy(pub_pl + poff, hdr_val, strlen(hdr_val));
    poff += strlen(hdr_val);
    memcpy(pub_pl + poff, msg, msg_len);
    poff += msg_len;

    send_frame_flags(pub_fd, CMQ_OP_PUBLISH, CMQ_FLAG_HEADERS, pub_pl, poff);
    wait_ms(100);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sp), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_MESSAGE);
    ASSERT(frame.hdr.flags & CMQ_FLAG_HEADERS);
    free_frame_payload(&frame);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(sp);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, info_has_stats) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 18905;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18905);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    ASSERT(frame.payload_len > 0);
    ASSERT(memchr(frame.payload, '{', frame.payload_len) != NULL);

    char *json = malloc(frame.payload_len + 1);
    memcpy(json, frame.payload, frame.payload_len);
    json[frame.payload_len] = '\0';
    ASSERT(strstr(json, "\"version\"") != NULL);
    ASSERT(strstr(json, "\"connections\"") != NULL);
    ASSERT(strstr(json, "\"subscriptions\"") != NULL);
    free(json);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
