#define _POSIX_C_SOURCE 200809L

#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

static int connect_from(const char *source, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    inet_pton(AF_INET, source, &local.sin_addr);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static int recv_connack(int fd, cmq_parser_t *parser) {
    for (int retry = 0; retry < 100; retry++) {
        const cmq_frame_t *frame = cmq_parser_frame(parser);
        if (frame) {
            int code = (frame->payload && frame->payload_len) ? frame->payload[0] : 255;
            cmq_parser_next(parser);
            return code;
        }
        uint8_t buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            cmq_parser_feed(parser, buf, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        } else {
            return -1;
        }
    }
    return -1;
}

static int failed_connect(const char *source, int port) {
    int fd = connect_from(source, port);
    if (fd < 0) return -1;
    uint8_t payload[] = {0, 1, 0, 5, 'u', 's', 'e', 'r', '!', 'b', 'a', 'd'};
    uint8_t wire[64];
    size_t wire_len = cmq_frame_encode(wire, sizeof(wire), CMQ_OP_CONNECT,
                                       0, payload, sizeof(payload));
    if (write(fd, wire, wire_len) != (ssize_t)wire_len) {
        close(fd);
        return -1;
    }
    cmq_parser_t *parser = cmq_parser_create();
    int code = parser ? recv_connack(fd, parser) : -1;
    cmq_parser_destroy(parser);
    close(fd);
    return code;
}

typedef struct {
    const char *source;
    int port;
    int code;
} failed_connect_worker_t;

static void *failed_connect_worker(void *arg) {
    failed_connect_worker_t *worker = arg;
    worker->code = failed_connect(worker->source, worker->port);
    return NULL;
}

static void start_server(cmq_server_t **srv_out, pthread_t *thread,
                         int port, int limit) {
    cmq_config_t config = {0};
    config.host = "127.0.0.1";
    config.port = port;
    config.num_threads = 1;
    config.log_to_stdout = 0;
    config.auth_username = "user";
    config.auth_password = "password";
    config.auth_failed_connects_per_sec = limit;
    ASSERT_EQ(cmq_server_create(srv_out, &config), CMQ_OK);
    ASSERT_EQ(pthread_create(thread, NULL, server_thread, *srv_out), 0);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
}

static void stop_server(cmq_server_t *srv, pthread_t thread) {
    cmq_server_stop(srv);
    pthread_join(thread, NULL);
    cmq_server_destroy(srv);
}

TEST(auth_rl, configurable_limit_uses_production_connect_path) {
    cmq_server_t *srv = NULL;
    pthread_t thread;
    start_server(&srv, &thread, 28901, 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28901), 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28901), 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28901), 4);
    stop_server(srv, thread);
}

TEST(auth_rl, different_source_ips_are_isolated) {
    cmq_server_t *srv = NULL;
    pthread_t thread;
    start_server(&srv, &thread, 28902, 1);
    ASSERT_EQ(failed_connect("127.0.0.1", 28902), 2);
    ASSERT_EQ(failed_connect("127.0.0.2", 28902), 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28902), 4);
    stop_server(srv, thread);
}

TEST(auth_rl, window_rolls_over) {
    cmq_server_t *srv = NULL;
    pthread_t thread;
    start_server(&srv, &thread, 28903, 1);
    ASSERT_EQ(failed_connect("127.0.0.1", 28903), 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28903), 4);
    struct timespec ts = {1, 100000000};
    nanosleep(&ts, NULL);
    ASSERT_EQ(failed_connect("127.0.0.1", 28903), 2);
    stop_server(srv, thread);
}

TEST(auth_rl, zero_disables_limiter) {
    cmq_server_t *srv = NULL;
    pthread_t thread;
    start_server(&srv, &thread, 28904, 0);
    ASSERT_EQ(failed_connect("127.0.0.1", 28904), 2);
    ASSERT_EQ(failed_connect("127.0.0.1", 28904), 2);
    stop_server(srv, thread);
}

TEST(auth_rl, concurrent_failures_do_not_bypass_limit) {
    enum { WORKERS = 8 };
    cmq_server_t *srv = NULL;
    pthread_t server;
    pthread_t workers[WORKERS];
    failed_connect_worker_t args[WORKERS];
    start_server(&srv, &server, 28905, 2);

    for (int i = 0; i < WORKERS; i++) {
        args[i].source = "127.0.0.1";
        args[i].port = 28905;
        args[i].code = -1;
        ASSERT_EQ(pthread_create(&workers[i], NULL, failed_connect_worker,
                                 &args[i]), 0);
    }
    for (int i = 0; i < WORKERS; i++)
        pthread_join(workers[i], NULL);
    ASSERT_EQ(failed_connect("127.0.0.1", 28905), 4);
    stop_server(srv, server);
}

TEST_MAIN()
