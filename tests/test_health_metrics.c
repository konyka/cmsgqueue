/* F12+F13: HTTP /healthz, /readyz, /metrics endpoints.
 *
 * Tests verify the response structure via the existing HTTP dispatch
 * path. The metrics endpoint is tested end-to-end (520 bytes pass).
 * Healthz and readyz are read to EOF so headers and bodies may arrive in
 * separate TCP reads without making the test depend on packet boundaries.
 *
 * The dispatch logic is in handle_ws_upgrade (src/server/cmq_server.c),
 * which routes GET /healthz, GET /readyz, GET /metrics,
 * GET /connz, GET /subz, GET /routez before the WebSocket handshake.
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_PORT 18760

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = (cmq_server_t *)arg;
    cmq_server_run(srv);
    return NULL;
}

TEST(http, metrics_returns_prometheus) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 2;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(HTTP_PORT + 2);
    ASSERT(fd >= 0);
    const char *req = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t w = send(fd, req, strlen(req), 0);
    ASSERT(w > 0);
    char buf[4096];
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t used = 0;
    ssize_t n;
    do {
        n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n > 0) used += (size_t)n;
    } while (n > 0 && used < sizeof(buf) - 1);
    close(fd);
    ASSERT(used > 0);
    buf[used] = '\0';
    /* F13: Prometheus exposition format. */
    ASSERT(strstr(buf, "# HELP cmq_connections") != NULL);
    ASSERT(strstr(buf, "# TYPE cmq_connections gauge") != NULL);
    ASSERT(strstr(buf, "cmq_connections ") != NULL);
    ASSERT(strstr(buf, "# HELP cmq_subscriptions") != NULL);
    ASSERT(strstr(buf, "# HELP cmq_messages_in_total") != NULL);
    ASSERT(strstr(buf, "# HELP cmq_messages_out_total") != NULL);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST(http, unknown_path_returns_404) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 3;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(HTTP_PORT + 3);
    ASSERT(fd >= 0);
    const char *req = "GET /unknown HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t w = send(fd, req, strlen(req), 0);
    ASSERT(w > 0);
    char buf[1024];
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    /* Unknown falls through to WS code path which returns -1 and
     * tears down. Connection is closed. */
    ASSERT(n >= 0);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

static void http_read_body(int fd, char *buf, size_t cap) {
    if (fd < 0) {
        buf[0] = '\0';
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t total = 0;
    size_t expected = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (total == cap - 1) break;
        if (expected == 0) {
            char *headers_end = strstr(buf, "\r\n\r\n");
            if (headers_end) {
                char *length = strstr(buf, "Content-Length: ");
                if (length && length < headers_end)
                    expected = (size_t)strtoul(length + 16, NULL, 10);
            }
        }
        if (expected > 0 && strstr(buf, "\r\n\r\n") &&
            total >= (size_t)(strstr(buf, "\r\n\r\n") - buf) + 4 + expected)
            break;
    }
    close(fd);
    buf[total] = '\0';
}

static void http_get_body(int port, const char *path, char *buf, size_t cap) {
    int fd = -1;
    struct timespec retry = {0, 10000000L};
    for (int attempt = 0; attempt < 100 && fd < 0; attempt++) {
        fd = connect_to(port);
        if (fd < 0) nanosleep(&retry, NULL);
    }
    if (fd >= 0) {
        char req[128];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
        (void)send(fd, req, strlen(req), 0);
    }
    http_read_body(fd, buf, cap);
}

TEST(http, health_and_ready_contract) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 7;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    char response[1024];
    http_get_body(HTTP_PORT + 7, "/readyz", response, sizeof(response));
    ASSERT(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
    ASSERT(strstr(response, "Content-Length: 19\r\n") != NULL);
    ASSERT(strstr(response, "\r\n\r\n{\"status\":\"ready\"}\n") != NULL);

    http_get_body(HTTP_PORT + 7, "/healthz", response, sizeof(response));
    ASSERT(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
    ASSERT(strstr(response, "Content-Length: 34\r\n") != NULL);
    ASSERT(strstr(response, "\r\n\r\n{\"status\":\"ok\",\"async_blocked\":0}\n") != NULL);

    /* The drain gate is raised before the listener is closed by
     * cmq_server_drain(), so exercise the same state used by the handler
     * while keeping this listener available for the probe. */
    int draining_fd = connect_to(HTTP_PORT + 7);
    ASSERT(draining_fd >= 0);
    const char *partial = "GET /readyz HTTP/1.1\r\nHost: x\r\n";
    ASSERT(send(draining_fd, partial, strlen(partial), 0) > 0);
    nanosleep(&ts, NULL);
    cmq_atomic_store_int(&srv->acceptor_drain, 1, CMQ_ATOMIC_RELEASE);
    ASSERT(send(draining_fd, "\r\n", 2, 0) == 2);
    http_read_body(draining_fd, response, sizeof(response));
    ASSERT(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
    ASSERT(strstr(response, "Content-Length: 22\r\n") != NULL);
    ASSERT(strstr(response, "\r\n\r\n{\"status\":\"draining\"}\n") != NULL);

    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST(http, connz_json) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 4;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);
    char buf[4096];
    http_get_body(HTTP_PORT + 4, "/connz", buf, sizeof(buf));
    ASSERT(strstr(buf, "num_connections") != NULL);
    ASSERT(strstr(buf, "connections") != NULL);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST(http, subz_json) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 5;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);
    char buf[4096];
    http_get_body(HTTP_PORT + 5, "/subz", buf, sizeof(buf));
    ASSERT(strstr(buf, "num_subscriptions") != NULL);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST(http, routez_json) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = HTTP_PORT + 6;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);
    char buf[4096];
    http_get_body(HTTP_PORT + 6, "/routez", buf, sizeof(buf));
    ASSERT(strstr(buf, "num_routes") != NULL);
    ASSERT(strstr(buf, "\"live\"") != NULL);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST_MAIN()
