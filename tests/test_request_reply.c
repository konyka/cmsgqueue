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
#include <time.h>

#define REQ_PORT 19900

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
    uint8_t buf[8192];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
    if (len == 0) return -1;
    return write(fd, buf, len);
}

static int recv_frame(int fd, cmq_frame_t *frame, cmq_parser_t *parser) {
    for (int retry = 0; retry < 200; retry++) {
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (f) {
            frame->hdr = f->hdr;
            frame->payload_len = f->payload_len;
            if (f->payload_len > 0 && f->payload) {
                frame->payload = malloc(f->payload_len);
                memcpy(frame->payload, f->payload, f->payload_len);
            } else { frame->payload = NULL; }
            cmq_parser_next(parser);
            return 0;
        }
        uint8_t buf[8192];
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

static void free_frame(cmq_frame_t *f) {
    free(f->payload);
    f->payload = NULL;
}

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    if (recv_frame(fd, &frame, parser) != 0) return;
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame(&frame);
        if (recv_frame(fd, &frame, parser) != 0) return;
    }
    if (frame.hdr.op == CMQ_OP_CONNACK) free_frame(&frame);
}

static int do_subscribe(int fd, cmq_parser_t *parser, const char *subject, uint32_t sub_id) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[256];
    buf[0] = (sub_id >> 24) & 0xFF;
    buf[1] = (sub_id >> 16) & 0xFF;
    buf[2] = (sub_id >> 8) & 0xFF;
    buf[3] = sub_id & 0xFF;
    buf[4] = (slen >> 8) & 0xFF;
    buf[5] = slen & 0xFF;
    memcpy(buf + 6, subject, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, buf, 6 + slen);
    wait_ms(50);
    cmq_frame_t f;
    if (recv_frame(fd, &f, parser) != 0) return -1;
    int ok = (f.hdr.op == CMQ_OP_SUBACK);
    free_frame(&f);
    return ok ? 0 : -1;
}

static void do_request(int fd, const char *subject, const char *reply_to, const char *msg) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint16_t rlen = (uint16_t)strlen(reply_to);
    size_t mlen = strlen(msg);
    uint8_t buf[4096];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen);
    off += slen;
    buf[off++] = (rlen >> 8) & 0xFF;
    buf[off++] = rlen & 0xFF;
    memcpy(buf + off, reply_to, rlen);
    off += rlen;
    memcpy(buf + off, msg, mlen);
    off += mlen;
    send_frame(fd, CMQ_OP_REQUEST, buf, off);
}

static void do_response(int fd, const char *subject, const char *msg) {
    uint16_t slen = (uint16_t)strlen(subject);
    size_t mlen = strlen(msg);
    uint8_t buf[4096];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen);
    off += slen;
    memcpy(buf + off, msg, mlen);
    off += mlen;
    send_frame(fd, CMQ_OP_RESPONSE, buf, off);
}

TEST(request_reply, basic_request_response) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = REQ_PORT;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int service_fd = connect_to(REQ_PORT);
    ASSERT(service_fd >= 0);
    wait_ms(20);
    cmq_parser_t *service_parser = cmq_parser_create();
    do_connect(service_fd, service_parser);
    ASSERT_EQ(do_subscribe(service_fd, service_parser, "rpc.add", 1), 0);

    int client_fd = connect_to(REQ_PORT);
    ASSERT(client_fd >= 0);
    wait_ms(20);
    cmq_parser_t *client_parser = cmq_parser_create();
    do_connect(client_fd, client_parser);

    int reply_fd = connect_to(REQ_PORT);
    ASSERT(reply_fd >= 0);
    wait_ms(20);
    cmq_parser_t *reply_parser = cmq_parser_create();
    do_connect(reply_fd, reply_parser);
    ASSERT_EQ(do_subscribe(reply_fd, reply_parser, "_INBOX.1", 2), 0);

    wait_ms(100);
    do_request(client_fd, "rpc.add", "_INBOX.1", "3+5");
    wait_ms(200);

    cmq_frame_t svc_msg;
    ASSERT_EQ(recv_frame(service_fd, &svc_msg, service_parser), 0);
    ASSERT_EQ(svc_msg.hdr.op, CMQ_OP_MESSAGE);
    free_frame(&svc_msg);

    do_response(service_fd, "_INBOX.1", "8");
    wait_ms(200);

    cmq_frame_t reply_msg;
    ASSERT_EQ(recv_frame(reply_fd, &reply_msg, reply_parser), 0);
    ASSERT_EQ(reply_msg.hdr.op, CMQ_OP_MESSAGE);
    free_frame(&reply_msg);

    close(service_fd);
    cmq_parser_destroy(service_parser);
    close(client_fd);
    cmq_parser_destroy(client_parser);
    close(reply_fd);
    cmq_parser_destroy(reply_parser);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(request_reply, multi_responder) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = REQ_PORT + 1;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int nsvc = 3;
    int svc_fds[4];
    cmq_parser_t *svc_parsers[4];
    for (int i = 0; i < nsvc; i++) {
        svc_fds[i] = connect_to(REQ_PORT + 1);
        ASSERT(svc_fds[i] >= 0);
        wait_ms(20);
        svc_parsers[i] = cmq_parser_create();
        do_connect(svc_fds[i], svc_parsers[i]);
        ASSERT_EQ(do_subscribe(svc_fds[i], svc_parsers[i], "rpc.echo", (uint32_t)(i + 1)), 0);
    }

    int client_fd = connect_to(REQ_PORT + 1);
    ASSERT(client_fd >= 0);
    wait_ms(20);
    cmq_parser_t *client_parser = cmq_parser_create();
    do_connect(client_fd, client_parser);
    wait_ms(100);

    int nrequests = 6;
    for (int i = 0; i < nrequests; i++) {
        do_request(client_fd, "rpc.echo", "_INBOX.resp", "ping");
        wait_ms(50);
    }
    wait_ms(300);

    int total_received = 0;
    for (int i = 0; i < nsvc; i++) {
        for (int attempt = 0; attempt < nrequests; attempt++) {
            cmq_frame_t f;
            if (recv_frame(svc_fds[i], &f, svc_parsers[i]) == 0 &&
                f.hdr.op == CMQ_OP_MESSAGE) {
                total_received++;
            }
            free_frame(&f);
        }
    }

    ASSERT_EQ(total_received, nsvc * nrequests);

    for (int i = 0; i < nsvc; i++) {
        close(svc_fds[i]);
        cmq_parser_destroy(svc_parsers[i]);
    }
    close(client_fd);
    cmq_parser_destroy(client_parser);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
