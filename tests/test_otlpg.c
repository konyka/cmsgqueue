/* v0.5.84: D1 OTLP/gRPC protobuf + HTTP/2 POST. */
#include "cmq_test.h"
#include "cmq_otlp.h"
#include "cmq_h2.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int has_mem(const uint8_t *h, size_t hn, const void *n, size_t nn) {
    if (!h || !n || nn == 0 || hn < nn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(h + i, n, nn) == 0) return 1;
    }
    return 0;
}

static int listen_loopback(int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
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

struct grab {
    int lfd;
    uint8_t buf[2048];
    int n;
};

static void *grab_thr(void *arg) {
    struct grab *g = arg;
    int c = accept(g->lfd, NULL, NULL);
    if (c < 0) return NULL;
    g->n = 0;
    while (g->n < (int)sizeof(g->buf)) {
        ssize_t r = recv(c, g->buf + g->n, sizeof(g->buf) - (size_t)g->n, 0);
        if (r <= 0) break;
        g->n += (int)r;
    }
    close(c);
    return NULL;
}

TEST(otlpg, encode_proto) {
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    memset(s.trace, 0xab, 16);
    s.kind = CMQ_OTEL_KIND_PUBLISH;
    s.t_ms = 42;
    uint8_t buf[CMQ_OTLP_JSON_MAX];
    int n = cmq_otlp_encode_proto(&s, 1, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(has_mem(buf, (size_t)n, "cmsgqueue", 9));
    ASSERT(has_mem(buf, (size_t)n, "service.name", 12));
    ASSERT(has_mem(buf, (size_t)n, "publish", 7));
    uint8_t tid[16];
    memset(tid, 0xab, 16);
    ASSERT(has_mem(buf, (size_t)n, tid, 16));
}

TEST(otlpg, grpc_frame) {
    uint8_t proto[8];
    memset(proto, 0x11, sizeof(proto));
    uint8_t out[16];
    int n = cmq_otlp_grpc_frame(proto, sizeof(proto), out, sizeof(out));
    ASSERT_EQ(n, 13);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 0);
    ASSERT_EQ(out[3], 0);
    ASSERT_EQ(out[4], 8);
    ASSERT(memcmp(out + 5, proto, 8) == 0);
}

TEST(otlpg, parse_and_post) {
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url("grpc://collector.local", &u), 0);
    ASSERT_EQ(u.grpc, 1);
    ASSERT_EQ(u.port, CMQ_OTLP_GRPC_PORT);
    ASSERT(strstr(u.path, "TraceService/Export") != NULL);
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct grab g;
    memset(&g, 0, sizeof(g));
    g.lfd = lfd;
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, grab_thr, &g), 0);
    char url[64];
    snprintf(url, sizeof(url), "grpc://127.0.0.1:%d", port);
    ASSERT_EQ(cmq_otlp_parse_url(url, &u), 0);
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    memset(s.trace, 0xcd, 16);
    s.kind = CMQ_OTEL_KIND_CONNECT;
    s.t_ms = 1;
    ASSERT_EQ(cmq_otlp_grpc_post(&u, &s), 0);
    pthread_join(tid, NULL);
    ASSERT(g.n >= (int)CMQ_H2_PREFACE_LEN);
    ASSERT_EQ(cmq_h2_preface_ok(g.buf, CMQ_H2_PREFACE_LEN), 0);
    ASSERT(has_mem(g.buf, (size_t)g.n, "application/grpc", 16));
    uint8_t tidb[16];
    memset(tidb, 0xcd, 16);
    ASSERT(has_mem(g.buf, (size_t)g.n, tidb, 16));
    close(lfd);
}

TEST(otlpg, reject) {
    cmq_otlp_url_t u;
    ASSERT(cmq_otlp_parse_url("grpc://user:pass@127.0.0.1", &u) != 0);
    ASSERT(cmq_otlp_parse_url("grpc://127.0.0.1/../x", &u) != 0);
    uint8_t buf[8];
    ASSERT(cmq_otlp_encode_proto(NULL, 1, buf, sizeof(buf)) < 0);
    ASSERT(cmq_otlp_grpc_frame(NULL, 1, buf, sizeof(buf)) < 0);
    ASSERT(cmq_otlp_grpc_post(NULL, NULL) != 0);
}

TEST_MAIN()
