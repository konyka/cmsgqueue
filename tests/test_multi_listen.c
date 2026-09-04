/* v0.5.24: real multi-listener runtime accept test. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define OPEN_PORT 23901  /* outside BOTH reserved ranges (28800-28999, 18800-18999) */

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static int wait_for_port(int port, int timeout_ms) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            return 0;
        }
        close(fd);
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
        elapsed += 10;
    }
    return -1;
}

static int try_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

TEST(multi_listener, three_listener_accept_all) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = OPEN_PORT;
    cfg.log_to_stdout = 0;
    cfg.listener_count = 3;
    cfg.listeners[0].tls_cert = NULL;
    cfg.listeners[0].tls_key = NULL;
    cfg.listeners[0].tls_ca = NULL;
    cfg.listeners[0].tls_verify_peer = 0;
    cfg.listeners[1] = cfg.listeners[0];
    cfg.listeners[2] = cfg.listeners[0];

    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    ASSERT_NOT_NULL(srv);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);

    ASSERT_EQ(wait_for_port(OPEN_PORT, 1000), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 1, 1000), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 2, 1000), 0);

    int f0 = try_connect(OPEN_PORT);
    ASSERT(f0 >= 0);
    close(f0);
    int f1 = try_connect(OPEN_PORT + 1);
    ASSERT(f1 >= 0);
    close(f1);
    int f2 = try_connect(OPEN_PORT + 2);
    ASSERT(f2 >= 0);
    close(f2);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(multi_listener, listener_count_one_unchanged) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = OPEN_PORT + 100;
    cfg.log_to_stdout = 0;
    cfg.listener_count = 1;

    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 100, 1000), 0);
    int f = try_connect(OPEN_PORT + 100);
    ASSERT(f >= 0);
    close(f);
    ASSERT(wait_for_port(OPEN_PORT + 101, 50) != 0);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(multi_listener, port_guard_excludes_test_range) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 28801;
    cfg.log_to_stdout = 0;
    cfg.listener_count = 3;

    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    ASSERT_EQ(wait_for_port(28801, 1000), 0);
    ASSERT(wait_for_port(28802, 50) != 0);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(multi_listener, three_listener_concurrent_connects) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = OPEN_PORT + 200;
    cfg.log_to_stdout = 0;
    cfg.listener_count = 3;

    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 200, 1000), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 201, 1000), 0);
    ASSERT_EQ(wait_for_port(OPEN_PORT + 202, 1000), 0);

    enum { N = 30 };
    int fds[N];
    for (int i = 0; i < N; i++) {
        fds[i] = try_connect(OPEN_PORT + 200 + (i % 3));
        ASSERT(fds[i] >= 0);
    }
    for (int i = 0; i < N; i++) close(fds[i]);

    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
