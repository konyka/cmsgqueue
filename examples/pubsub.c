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
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int connect_to_server(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
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

static int recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int read_payload(int fd, uint8_t *buf, size_t buf_size,
                         uint8_t *payload_out, size_t *payload_len_out) {
    uint8_t hdr_buf[9];
    if (recv_exact(fd, hdr_buf, sizeof(hdr_buf)) != 0) return -1;

    cmq_frame_hdr_t hdr;
    memcpy(&hdr, hdr_buf, sizeof(hdr));
    uint32_t plen = (uint32_t)hdr_buf[5] | ((uint32_t)hdr_buf[6] << 8) |
                    ((uint32_t)hdr_buf[7] << 16) | ((uint32_t)hdr_buf[8] << 24);

    if (plen > buf_size) return -1;
    if (plen > 0 && recv_exact(fd, buf, plen) != 0) return -1;

    if (payload_out && payload_len_out) {
        memcpy(payload_out, buf, plen);
        *payload_len_out = plen;
    }
    return (int)hdr.op;
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = 7654;
    const char *mode = "both";

    if (argc > 1) mode = argv[1];
    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);

    printf("CMSGQueue Pub/Sub Example v%s\n", cmq_version());
    printf("Usage: %s [publish|subscribe|both] [host] [port]\n\n", argv[0]);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (strcmp(mode, "publish") == 0) {
        printf("Connecting to %s:%d as publisher...\n", host, port);
        int fd = connect_to_server(host, port);
        if (fd < 0) { fprintf(stderr, "Connection failed\n"); return 1; }

        uint8_t buf[4096];
        size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNECT, 0, NULL, 0);
        send_all(fd, buf, len);

        uint8_t payload[4096];
        size_t plen = 0;
        int op = read_payload(fd, payload, sizeof(payload), payload, &plen);
        if (op == CMQ_OP_CONNACK) printf("Connected (CONNACK received).\n");

        int count = 0;
        while (g_running && count < 100) {
            char msg[256];
            snprintf(msg, sizeof(msg), "message #%d", count);
            len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_PUBLISH, 0,
                                    (const uint8_t *)msg, strlen(msg));
            send_all(fd, buf, len);
            printf("Published: %s\n", msg);
            count++;
            { struct timeval tv = {0, 500000}; select(0, NULL, NULL, NULL, &tv); }
        }
        close(fd);
        printf("Publisher done. Sent %d messages.\n", count);

    } else if (strcmp(mode, "subscribe") == 0) {
        printf("Connecting to %s:%d as subscriber...\n", host, port);
        int fd = connect_to_server(host, port);
        if (fd < 0) { fprintf(stderr, "Connection failed\n"); return 1; }

        uint8_t buf[4096];
        size_t len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNECT, 0, NULL, 0);
        send_all(fd, buf, len);

        uint8_t payload[4096];
        size_t plen = 0;
        read_payload(fd, payload, sizeof(payload), payload, &plen);
        printf("Connected.\n");

        char sub[] = "example.pubsub";
        len = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_SUBSCRIBE, 0,
                                (const uint8_t *)sub, strlen(sub));
        send_all(fd, buf, len);
        printf("Subscribed to 'example.pubsub'. Waiting for messages...\n");

        while (g_running) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {1, 0};
            int r = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (r <= 0) continue;

            uint8_t rbuf[4096];
            size_t rlen = 0;
            int op = read_payload(fd, rbuf, sizeof(rbuf), payload, &rlen);
            if (op == CMQ_OP_MESSAGE) {
                printf("Received message: %.*s\n", (int)rlen, payload);
            } else if (op < 0) {
                break;
            }
        }
        close(fd);
        printf("Subscriber done.\n");

    } else {
        printf("Starting server on %s:%d...\n", host, port);
        cmq_config_t config = {0};
        config.host = host;
        config.port = port;
        config.num_threads = 1;
        config.max_clients = 100;
        config.log_to_stdout = 1;
        config.log_level = 2;

        cmq_server_t *server = NULL;
        cmq_status_t rc = cmq_server_create(&server, &config);
        if (rc != CMQ_OK) {
            fprintf(stderr, "Failed to create server: %d\n", rc);
            return 1;
        }
        printf("Server started. Press Ctrl+C to stop.\n");
        cmq_server_run(server);
        cmq_server_destroy(server);
        printf("Server stopped.\n");
    }
    return 0;
}
