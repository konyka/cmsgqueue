/* v0.5.86: leaf/gateway CONNECT/CONNACK e2e. */
#include "cmq_test.h"
#include "cmq_leaf.h"
#include "cmq_gateway.h"
#include "cmq_parser.h"
#include "cmq_proto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int listen_loopback(int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 2) != 0) {
        close(s);
        return -1;
    }
    socklen_t alen = sizeof(a);
    if (getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        close(s);
        return -1;
    }
    *port = (int)ntohs(a.sin_port);
    return s;
}

static int write_connack(int fd, uint8_t code) {
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), CMQ_OP_CONNACK, 0, &code, 1);
    if (n == 0) return -1;
    return send(fd, buf, n, 0) == (ssize_t)n ? 0 : -1;
}

struct hub {
    int lfd;
    uint8_t code;
    int saw_connect;
    atomic_int done;
};

static void *hub_thr(void *arg) {
    struct hub *h = arg;
    int c = accept(h->lfd, NULL, NULL);
    if (c < 0) return NULL;
    uint8_t buf[256];
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    if (n >= 5 && buf[0] == CMQ_PROTO_MAGIC_0 && buf[1] == CMQ_PROTO_MAGIC_1 &&
        buf[4] == (uint8_t)CMQ_OP_CONNECT)
        h->saw_connect = 1;
    (void)write_connack(c, h->code);
    /* Keep TCP up so leaf_is_connected / gw live-peer probes succeed. */
    int waited = 0;
    while (!atomic_load_explicit(&h->done, memory_order_acquire) &&
           waited < 5000) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        waited++;
    }
    close(c);
    return NULL;
}

static void hub_finish(struct hub *h, pthread_t tid, int lfd) {
    atomic_store_explicit(&h->done, 1, memory_order_release);
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(leafe, leaf_connect) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct hub h = {.lfd = lfd, .code = 0};
    atomic_init(&h.done, 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, hub_thr, &h), 0);
    cmq_leaf_node_t *leaf = cmq_leaf_create("127.0.0.1", port);
    ASSERT_NOT_NULL(leaf);
    ASSERT_EQ(cmq_leaf_connect(leaf), 0);
    ASSERT_EQ(cmq_leaf_is_connected(leaf), 1);
    ASSERT_EQ(cmq_leaf_connect(leaf), 0);
    ASSERT_EQ(h.saw_connect, 1);
    cmq_leaf_destroy(leaf);
    hub_finish(&h, tid, lfd);
}

TEST(leafe, gateway_connect) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct hub h = {.lfd = lfd, .code = 0};
    atomic_init(&h.done, 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, hub_thr, &h), 0);
    cmq_gateway_t *gw = cmq_gateway_create("local");
    ASSERT_NOT_NULL(gw);
    ASSERT_EQ(cmq_gateway_add_remote(gw, "remote", "127.0.0.1", port), 0);
    ASSERT_EQ(cmq_gateway_connect_remote(gw, "remote"), 0);
    ASSERT_EQ(cmq_gateway_connection_count(gw), 1u);
    ASSERT_EQ(cmq_gateway_connect_remote(gw, "remote"), 0);
    ASSERT_EQ(h.saw_connect, 1);
    cmq_gateway_destroy(gw);
    hub_finish(&h, tid, lfd);
}

TEST(leafe, bad_connack) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct hub h = {.lfd = lfd, .code = 1};
    atomic_init(&h.done, 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, hub_thr, &h), 0);
    cmq_leaf_node_t *leaf = cmq_leaf_create("127.0.0.1", port);
    ASSERT(cmq_leaf_connect(leaf) != 0);
    ASSERT_EQ(cmq_leaf_is_connected(leaf), 0);
    cmq_leaf_destroy(leaf);
    hub_finish(&h, tid, lfd);
}

TEST(leafe, reject) {
    ASSERT(cmq_leaf_connect(NULL) != 0);
    ASSERT(cmq_gateway_connect_remote(NULL, "x") != 0);
    ASSERT_EQ(cmq_leaf_is_connected(NULL), 0);
    ASSERT_EQ(cmq_frame_encode(NULL, 0, CMQ_OP_CONNACK, 0, NULL, 0), 0u);
}

TEST_MAIN()
