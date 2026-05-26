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

#define CORO_PORT 19800

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

static void do_publish(int fd, const char *subject, const char *msg) {
    uint16_t slen = (uint16_t)strlen(subject);
    size_t mlen = strlen(msg);
    uint8_t buf[4096];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen);
    off += slen;
    buf[off++] = 0;
    buf[off++] = 0;
    memcpy(buf + off, msg, mlen);
    off += mlen;
    send_frame(fd, CMQ_OP_PUBLISH, buf, off);
}

TEST(coro_integration, high_fanout_coro_delivery) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = CORO_PORT;
    config.log_to_stdout = 0;
    config.num_threads = 2;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(300);

    int nsubs = 20;
    int fds[24];
    cmq_parser_t *parsers[24];
    (void)memset(fds, 0, sizeof(fds));
    (void)memset(parsers, 0, sizeof(parsers));

    for (int i = 0; i < nsubs; i++) {
        fds[i] = connect_to(CORO_PORT);
        ASSERT(fds[i] >= 0);
        wait_ms(20);
        parsers[i] = cmq_parser_create();
        do_connect(fds[i], parsers[i]);
        ASSERT_EQ(do_subscribe(fds[i], parsers[i], "coro.test", (uint32_t)(i + 1)), 0);
    }

    int pub_fd = connect_to(CORO_PORT);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(100);

    do_publish(pub_fd, "coro.test", "hello-from-coroutine");
    wait_ms(500);

    int received = 0;
    for (int i = 0; i < nsubs; i++) {
        cmq_frame_t f;
        if (recv_frame(fds[i], &f, parsers[i]) == 0 && f.hdr.op == CMQ_OP_MESSAGE) {
            received++;
        }
        free_frame(&f);
    }

    ASSERT_EQ(received, nsubs);

    for (int i = 0; i < nsubs; i++) {
        close(fds[i]);
        cmq_parser_destroy(parsers[i]);
    }
    close(pub_fd);
    cmq_parser_destroy(pub_parser);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(coro_integration, multi_publish_fanout) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = CORO_PORT + 1;
    config.log_to_stdout = 0;
    config.num_threads = 4;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(300);

    int nsubs = 20;
    int npubs = 5;
    int fds[32];
    cmq_parser_t *parsers[32];
    (void)memset(fds, 0, sizeof(fds));
    (void)memset(parsers, 0, sizeof(parsers));

    for (int i = 0; i < nsubs; i++) {
        fds[i] = connect_to(CORO_PORT + 1);
        ASSERT(fds[i] >= 0);
        wait_ms(10);
        parsers[i] = cmq_parser_create();
        do_connect(fds[i], parsers[i]);
        ASSERT_EQ(do_subscribe(fds[i], parsers[i], "coro.multi", (uint32_t)(i + 1)), 0);
    }

    for (int i = 0; i < npubs; i++) {
        int idx = nsubs + i;
        fds[idx] = connect_to(CORO_PORT + 1);
        ASSERT(fds[idx] >= 0);
        wait_ms(10);
        parsers[idx] = cmq_parser_create();
        do_connect(fds[idx], parsers[idx]);
    }
    wait_ms(100);

    for (int p = 0; p < npubs; p++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "msg-%d", p);
        do_publish(fds[nsubs + p], "coro.multi", msg);
        wait_ms(50);
    }
    wait_ms(1000);

    int total_received = 0;
    for (int i = 0; i < nsubs; i++) {
        for (int m = 0; m < npubs; m++) {
            cmq_frame_t f;
            if (recv_frame(fds[i], &f, parsers[i]) == 0 && f.hdr.op == CMQ_OP_MESSAGE) {
                total_received++;
            }
            free_frame(&f);
        }
    }

    ASSERT_EQ(total_received, nsubs * npubs);

    for (int i = 0; i < nsubs + npubs; i++) {
        close(fds[i]);
        cmq_parser_destroy(parsers[i]);
    }

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
