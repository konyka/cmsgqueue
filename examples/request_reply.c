#define _POSIX_C_SOURCE 200809L
#include "cmq.h"
#include "cmq_proto.h"
#include "cmq_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static volatile int g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    return fd;
}

static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static void send_frame(int fd, cmq_op_t op, const uint8_t *payload, size_t plen) {
    uint8_t buf[8192];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
    if (len > 0) send_all(fd, buf, len);
}

static int recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int read_response(int fd, uint8_t *buf, size_t buf_size) {
    uint8_t hdr_buf[9];
    if (recv_exact(fd, hdr_buf, 9) != 0) return -1;
    uint32_t plen = (uint32_t)hdr_buf[5] | ((uint32_t)hdr_buf[6] << 8) |
                    ((uint32_t)hdr_buf[7] << 16) | ((uint32_t)hdr_buf[8] << 24);
    if (plen > buf_size) return -1;
    if (plen > 0 && recv_exact(fd, buf, plen) != 0) return -1;
    return (int)hdr_buf[4];
}

static void do_connect(int fd) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    uint8_t buf[256];
    int op = read_response(fd, buf, sizeof(buf));
    if (op == CMQ_OP_INFO) op = read_response(fd, buf, sizeof(buf));
    if (op == CMQ_OP_CONNACK) printf("  Connected (CONNACK)\n");
}

static void do_subscribe(int fd, const char *subject, uint32_t sub_id) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[256];
    size_t off = 0;
    buf[off++] = (sub_id >> 24) & 0xFF;
    buf[off++] = (sub_id >> 16) & 0xFF;
    buf[off++] = (sub_id >> 8) & 0xFF;
    buf[off++] = sub_id & 0xFF;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen);
    off += slen;
    send_frame(fd, CMQ_OP_SUBSCRIBE, buf, off);
    uint8_t rbuf[256];
    read_response(fd, rbuf, sizeof(rbuf));
    printf("  Subscribed to '%s' (id=%u)\n", subject, sub_id);
}

static void do_request(int fd, const char *subject, const char *reply_to, const char *msg) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint16_t rlen = (uint16_t)strlen(reply_to);
    size_t mlen = strlen(msg);
    uint8_t buf[4096];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen); off += slen;
    buf[off++] = (rlen >> 8) & 0xFF;
    buf[off++] = rlen & 0xFF;
    memcpy(buf + off, reply_to, rlen); off += rlen;
    memcpy(buf + off, msg, mlen); off += mlen;
    send_frame(fd, CMQ_OP_REQUEST, buf, off);
}

static void do_response(int fd, const char *subject, const char *msg) {
    uint16_t slen = (uint16_t)strlen(subject);
    size_t mlen = strlen(msg);
    uint8_t buf[4096];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen); off += slen;
    memcpy(buf + off, msg, mlen); off += mlen;
    send_frame(fd, CMQ_OP_RESPONSE, buf, off);
}

static void do_stats(int fd) {
    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    uint8_t buf[256];
    int op = read_response(fd, buf, sizeof(buf));
    if (op == CMQ_OP_STATS) {
        uint64_t conn = 0, msgs_in = 0, msgs_out = 0;
        for (int b = 0; b < 8; b++) conn = (conn << 8) | buf[b];
        for (int b = 0; b < 8; b++) msgs_in = (msgs_in << 8) | buf[8 + b];
        for (int b = 0; b < 8; b++) msgs_out = (msgs_out << 8) | buf[16 + b];
        printf("  Stats: connections=%llu msgs_in=%llu msgs_out=%llu\n",
               (unsigned long long)conn, (unsigned long long)msgs_in,
               (unsigned long long)msgs_out);
    }
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = 7654;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    printf("CMSGQueue Request-Reply Example v%s\n", cmq_version());
    printf("Usage: %s [host] [port]\n\n", argv[0]);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("[Service] Connecting to %s:%d...\n", host, port);
    int svc_fd = connect_to(host, port);
    if (svc_fd < 0) { fprintf(stderr, "Failed to connect\n"); return 1; }
    do_connect(svc_fd);
    do_subscribe(svc_fd, "rpc.add", 1);

    printf("[Client] Connecting to %s:%d...\n", host, port);
    int cli_fd = connect_to(host, port);
    if (cli_fd < 0) { fprintf(stderr, "Failed to connect\n"); return 1; }
    do_connect(cli_fd);
    do_subscribe(cli_fd, "_INBOX.1", 2);

    printf("[Reply] Connecting to %s:%d...\n", host, port);
    int reply_fd = connect_to(host, port);
    if (reply_fd < 0) { fprintf(stderr, "Failed to connect\n"); return 1; }
    do_connect(reply_fd);
    do_subscribe(reply_fd, "_INBOX.1", 3);

    nanosleep(&(struct timespec){0, 200000000L}, NULL);

    printf("\n--- Sending request: 3+5 ---\n");
    do_request(cli_fd, "rpc.add", "_INBOX.1", "3+5");
    nanosleep(&(struct timespec){0, 100000000L}, NULL);

    uint8_t msg_buf[4096];
    int op = read_response(svc_fd, msg_buf, sizeof(msg_buf));
    if (op == CMQ_OP_MESSAGE) {
        printf("[Service] Received request, sending response: 8\n");
        do_response(svc_fd, "_INBOX.1", "8");
    }

    nanosleep(&(struct timespec){0, 100000000L}, NULL);
    op = read_response(reply_fd, msg_buf, sizeof(msg_buf));
    if (op == CMQ_OP_MESSAGE) {
        printf("[Reply] Received response!\n");
    }

    printf("\n--- Server Stats ---\n");
    do_stats(cli_fd);

    printf("\nDone.\n");
    close(svc_fd);
    close(cli_fd);
    close(reply_fd);
    return 0;
}
