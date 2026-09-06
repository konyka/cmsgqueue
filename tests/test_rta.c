/* v0.5.147: reload attaches routes when create had none. */
#include "cmq_cluster.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_route.h"
#include "cmq_test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
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

TEST(rta, apply) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct hub h = {.lfd = lfd, .code = 0};
    atomic_init(&h.done, 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, hub_thr, &h), 0);

    cmq_cluster_t *cl = cmq_cluster_create("c1", "n1");
    ASSERT_NOT_NULL(cl);
    cmq_route_pool_t *pool = cmq_route_pool_create(cl);
    ASSERT_NOT_NULL(pool);
    char *live = NULL;
    int live_port = 0;
    ASSERT_EQ(cmq_route_reload_attach(pool, "r0",
                                      (const char **)&live, &live_port,
                                      "127.0.0.1", port, NULL, NULL), 0);
    ASSERT_STR_EQ(live, "127.0.0.1");
    ASSERT_EQ(live_port, port);
    ASSERT(cmq_route_target_count(pool) >= 1u);
    cmq_route_conn_t snap;
    ASSERT_EQ(cmq_route_get_conn(pool, "r0", &snap), 0);
    ASSERT(snap.fd >= 0);
    int keep_fd = snap.fd;
    ASSERT_EQ(h.saw_connect, 1);

    ASSERT_EQ(cmq_route_reload_attach(pool, "r0",
                                      (const char **)&live, &live_port,
                                      "10.0.0.1", port + 1, NULL, NULL), 0);
    ASSERT_STR_EQ(live, "127.0.0.1");
    ASSERT_EQ(live_port, port);
    ASSERT_EQ(cmq_route_get_conn(pool, "r0", &snap), 0);
    ASSERT_EQ(snap.fd, keep_fd);

    cmq_route_pool_destroy(pool);
    cmq_cluster_destroy(cl);
    free(live);
    hub_finish(&h, tid, lfd);
}

TEST(rta, omitted) {
    cmq_cluster_t *cl = cmq_cluster_create("c1", "n1");
    ASSERT_NOT_NULL(cl);
    cmq_route_pool_t *pool = cmq_route_pool_create(cl);
    ASSERT_NOT_NULL(pool);
    char *live = NULL;
    int port = 0;
    ASSERT_EQ(cmq_route_reload_attach(pool, "r0",
                                      (const char **)&live, &port,
                                      NULL, 0, NULL, NULL), 0);
    ASSERT(live == NULL);
    ASSERT_EQ(cmq_route_target_count(pool), 0u);
    cmq_route_pool_destroy(pool);
    cmq_cluster_destroy(cl);
}

TEST(rta, empty) {
    cmq_cluster_t *cl = cmq_cluster_create("c1", "n1");
    ASSERT_NOT_NULL(cl);
    cmq_route_pool_t *pool = cmq_route_pool_create(cl);
    ASSERT_NOT_NULL(pool);
    char *live = NULL;
    int port = 4223;
    ASSERT_EQ(cmq_route_reload_attach(pool, "r0",
                                      (const char **)&live, &port,
                                      "", 0, NULL, NULL), 0);
    ASSERT(live == NULL);
    ASSERT_EQ(cmq_route_target_count(pool), 0u);
    cmq_route_pool_destroy(pool);
    cmq_cluster_destroy(cl);
}

TEST(rta, reject) {
    cmq_cluster_t *cl = cmq_cluster_create("c1", "n1");
    ASSERT_NOT_NULL(cl);
    cmq_route_pool_t *pool = cmq_route_pool_create(cl);
    ASSERT_NOT_NULL(pool);
    char *live = strdup("10.0.0.3");
    int port = 4223;
    ASSERT(cmq_route_reload_attach(pool, "r0",
                                   (const char **)&live, &port,
                                   "localhost", 4222, NULL, NULL) != 0);
    ASSERT_STR_EQ(live, "10.0.0.3");
    ASSERT_EQ(port, 4223);
    ASSERT(cmq_route_reload_attach(NULL, "r0",
                                   (const char **)&live, &port,
                                   "10.0.0.9", 4222, NULL, NULL) != 0);
    ASSERT(cmq_route_reload_attach(pool, "r0",
                                   (const char **)&live, &port,
                                   "10.0.0.9", 65536, NULL, NULL) != 0);
    ASSERT_STR_EQ(live, "10.0.0.3");
    ASSERT_EQ(cmq_route_target_count(pool), 0u);
    cmq_route_pool_destroy(pool);
    cmq_cluster_destroy(cl);
    free(live);
}

TEST_MAIN()
