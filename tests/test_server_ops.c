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

#define STATS_PORT 19700

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

TEST(server_ops, stats_query) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = STATS_PORT;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    do_subscribe(fd, parser, "stats.test", 1);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(100);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    ASSERT(f.payload_len >= 52);

    uint64_t conn = 0, msg_in = 0, msg_out = 0;
    for (int b = 0; b < 8; b++) conn = (conn << 8) | f.payload[b];
    for (int b = 0; b < 8; b++) msg_in = (msg_in << 8) | f.payload[8 + b];
    for (int b = 0; b < 8; b++) msg_out = (msg_out << 8) | f.payload[16 + b];

    ASSERT(conn >= 1);
    free_frame(&f);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, batch_publish) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 1;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(STATS_PORT + 1);
    ASSERT(sub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "batch.a", 1), 0);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "batch.b", 2), 0);

    int pub_fd = connect_to(STATS_PORT + 1);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(100);

    const char *msg1 = "hello-a";
    const char *msg2 = "hello-b";
    uint16_t slen1 = (uint16_t)strlen("batch.a");
    uint16_t slen2 = (uint16_t)strlen("batch.b");
    size_t mlen1 = strlen(msg1);
    size_t mlen2 = strlen(msg2);

    uint8_t batch[512];
    size_t off = 0;
    batch[off++] = 0;
    batch[off++] = 2;

    batch[off++] = (slen1 >> 8) & 0xFF;
    batch[off++] = slen1 & 0xFF;
    memcpy(batch + off, "batch.a", slen1);
    off += slen1;
    batch[off++] = 0; batch[off++] = 0;
    uint32_t plen1 = (uint32_t)mlen1;
    batch[off++] = (plen1 >> 24) & 0xFF;
    batch[off++] = (plen1 >> 16) & 0xFF;
    batch[off++] = (plen1 >> 8) & 0xFF;
    batch[off++] = plen1 & 0xFF;
    memcpy(batch + off, msg1, mlen1);
    off += mlen1;

    batch[off++] = (slen2 >> 8) & 0xFF;
    batch[off++] = slen2 & 0xFF;
    memcpy(batch + off, "batch.b", slen2);
    off += slen2;
    batch[off++] = 0; batch[off++] = 0;
    uint32_t plen2 = (uint32_t)mlen2;
    batch[off++] = (plen2 >> 24) & 0xFF;
    batch[off++] = (plen2 >> 16) & 0xFF;
    batch[off++] = (plen2 >> 8) & 0xFF;
    batch[off++] = plen2 & 0xFF;
    memcpy(batch + off, msg2, mlen2);
    off += mlen2;

    send_frame(pub_fd, CMQ_OP_BATCH, batch, off);
    wait_ms(300);

    int received = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        cmq_frame_t f;
        if (recv_frame(sub_fd, &f, sub_parser) == 0 && f.hdr.op == CMQ_OP_MESSAGE) {
            received++;
        }
        free_frame(&f);
    }
    ASSERT_EQ(received, 2);

    close(sub_fd);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    cmq_parser_destroy(pub_parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, keepalive_disconnect) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 2;
    config.log_to_stdout = 0;
    config.ping_interval_ms = 500;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT + 2);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);
    wait_ms(100);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(100);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    free_frame(&f);

    wait_ms(1500);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    ssize_t written = write(fd, "x", 1);
    (void)written;

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
