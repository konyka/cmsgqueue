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
    wait_ms(100);
    cmq_frame_t frame;
    if (recv_frame(fd, &frame, parser) != 0) return;
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    if (frame.hdr.op == CMQ_OP_CONNACK) {
        free_frame_payload(&frame);
    }
}

TEST(worker, cross_thread_pubsub) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19201;
    config.num_threads = 2;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(19201);
    ASSERT(sub_fd >= 0);
    wait_ms(100);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);

    const char *subject = "worker.test";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    uint32_t sub_id = 1;
    sub_pl[0] = (sub_id >> 24) & 0xFF;
    sub_pl[1] = (sub_id >> 16) & 0xFF;
    sub_pl[2] = (sub_id >> 8) & 0xFF;
    sub_pl[3] = sub_id & 0xFF;
    sub_pl[4] = (slen >> 8) & 0xFF;
    sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    wait_ms(100);

    cmq_frame_t suback;
    ASSERT_EQ(recv_frame(sub_fd, &suback, sub_parser), 0);
    ASSERT_EQ(suback.hdr.op, CMQ_OP_SUBACK);
    free_frame_payload(&suback);

    int pub_fd = connect_to(19201);
    ASSERT(pub_fd >= 0);
    wait_ms(100);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);

    uint8_t pub_pl[256];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0;
    pub_pl[off++] = 0;
    const char *msg = "hello workers";
    memcpy(pub_pl + off, msg, strlen(msg));
    off += strlen(msg);
    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
    wait_ms(300);

    cmq_frame_t msg_frame;
    ASSERT_EQ(recv_frame(sub_fd, &msg_frame, sub_parser), 0);
    ASSERT_EQ(msg_frame.hdr.op, CMQ_OP_MESSAGE);
    free_frame_payload(&msg_frame);

    cmq_parser_destroy(pub_parser);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(worker, multi_pub_single_sub) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = 19202;
    config.num_threads = 3;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(19202);
    ASSERT(sub_fd >= 0);
    wait_ms(100);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);

    const char *subject = "multi.test";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    uint32_t sub_id = 1;
    sub_pl[0] = (sub_id >> 24) & 0xFF;
    sub_pl[1] = (sub_id >> 16) & 0xFF;
    sub_pl[2] = (sub_id >> 8) & 0xFF;
    sub_pl[3] = sub_id & 0xFF;
    sub_pl[4] = (slen >> 8) & 0xFF;
    sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    wait_ms(100);

    cmq_frame_t suback;
    ASSERT_EQ(recv_frame(sub_fd, &suback, sub_parser), 0);
    ASSERT_EQ(suback.hdr.op, CMQ_OP_SUBACK);
    free_frame_payload(&suback);

    for (int i = 0; i < 5; i++) {
        int pub_fd = connect_to(19202);
        ASSERT(pub_fd >= 0);
        wait_ms(50);
        cmq_parser_t *pub_parser = cmq_parser_create();
        do_connect(pub_fd, pub_parser);

        uint8_t pub_pl[256];
        size_t off2 = 0;
        pub_pl[off2++] = (slen >> 8) & 0xFF;
        pub_pl[off2++] = slen & 0xFF;
        memcpy(pub_pl + off2, subject, slen);
        off2 += slen;
        pub_pl[off2++] = 0;
        pub_pl[off2++] = 0;
        char msg[64];
        int mlen = snprintf(msg, sizeof(msg), "msg-%d", i);
        memcpy(pub_pl + off2, msg, (size_t)mlen);
        off2 += (size_t)mlen;
        send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off2);
        wait_ms(50);
        close(pub_fd);
        cmq_parser_destroy(pub_parser);
    }
    wait_ms(300);

    int received = 0;
    for (int i = 0; i < 5; i++) {
        cmq_frame_t msg_frame;
        if (recv_frame(sub_fd, &msg_frame, sub_parser) == 0) {
            ASSERT_EQ(msg_frame.hdr.op, CMQ_OP_MESSAGE);
            free_frame_payload(&msg_frame);
            received++;
        }
    }
    ASSERT_EQ(received, 5);

    cmq_parser_destroy(sub_parser);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
