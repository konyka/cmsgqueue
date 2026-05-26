#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define DEFAULT_PORT 7654
#define DEFAULT_MSGS 10000
#define DEFAULT_CLIENTS 10
#define MSG_SIZE 64

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
    for (int retry = 0; retry < 100; retry++) {
        ssize_t n = write(fd, buf, len);
        if (n > 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }
    return -1;
}

static int recv_frame(int fd, cmq_parser_t *parser) {
    for (int retry = 0; retry < 200; retry++) {
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (f) { cmq_parser_next(parser); return 0; }
        uint8_t buf[8192];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        cmq_parser_feed(parser, buf, (size_t)n);
    }
    return -1;
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);
    for (int i = 0; i < 5; i++) {
        if (recv_frame(fd, parser) == 0) break;
    }
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    int nclients = DEFAULT_CLIENTS;
    int nmsgs = DEFAULT_MSGS;
    int nthreads = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) nclients = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) nmsgs = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) nthreads = atoi(argv[++i]);
    }

    printf("CMSGQueue Benchmark\n");
    printf("  port=%d clients=%d msgs=%d threads=%d\n", port, nclients, nmsgs, nthreads);

    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = port;
    config.num_threads = nthreads;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    cmq_server_create(&srv, &config);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec tw = {0, 200000000};
    nanosleep(&tw, NULL);

    int *pub_fds = malloc((size_t)nclients * sizeof(int));
    cmq_parser_t **pub_parsers = malloc((size_t)nclients * sizeof(cmq_parser_t *));

    for (int i = 0; i < nclients; i++) {
        pub_fds[i] = connect_to(port);
        pub_parsers[i] = cmq_parser_create();
        struct timespec tu = {0, 10000000};
        nanosleep(&tu, NULL);
        do_connect(pub_fds[i], pub_parsers[i]);
    }

    int sub_fd = connect_to(port);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);

    const char *subject = "bench.topic";
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
    struct timespec ts50 = {0, 50000000};
    nanosleep(&ts50, NULL);
    recv_frame(sub_fd, sub_parser);

    char msg[MSG_SIZE];
    memset(msg, 'x', MSG_SIZE - 1);
    msg[MSG_SIZE - 1] = '\0';
    size_t msg_len = strlen(msg);

    printf("Publishing %d messages across %d clients...\n", nmsgs, nclients);
    double t0 = now_sec();

    for (int i = 0; i < nmsgs; i++) {
        int ci = i % nclients;
        uint8_t pub_pl[4096];
        size_t off = 0;
        pub_pl[off++] = (slen >> 8) & 0xFF;
        pub_pl[off++] = slen & 0xFF;
        memcpy(pub_pl + off, subject, slen);
        off += slen;
        pub_pl[off++] = 0;
        pub_pl[off++] = 0;
        memcpy(pub_pl + off, msg, msg_len);
        off += msg_len;
        send_frame(pub_fds[ci], CMQ_OP_PUBLISH, pub_pl, off);
    }

    double t_pub = now_sec() - t0;
    printf("  Publish time: %.3fs (%.0f msg/s)\n", t_pub, (double)nmsgs / t_pub);

    int received = 0;
    double t_recv_start = now_sec();
    double timeout = 30.0;
    while (received < nmsgs && (now_sec() - t_recv_start) < timeout) {
        if (recv_frame(sub_fd, sub_parser) == 0) {
            received++;
        } else {
            break;
        }
    }
    double t_total = now_sec() - t0;

    printf("  Received: %d/%d\n", received, nmsgs);
    printf("  Total time: %.3fs\n", t_total);
    if (received > 0) {
        printf("  End-to-end: %.0f msg/s\n", (double)received / t_total);
        printf("  Avg latency: %.3f ms\n", t_total * 1000.0 / (double)received);
    }

    for (int i = 0; i < nclients; i++) {
        close(pub_fds[i]);
        cmq_parser_destroy(pub_parsers[i]);
    }
    free(pub_fds);
    free(pub_parsers);
    cmq_parser_destroy(sub_parser);
    close(sub_fd);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);

    printf("Done.\n");
    return 0;
}
