/* v0.5.82: D3 JWKS refresh. */
#include "cmq_test.h"
#include "cmq_jwksf.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *k1 =
    "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"k\":\"czNjcmV0\"}]}";
static const char *k2 =
    "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k2\",\"k\":\"dG9rZW4x\"}]}";

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

static void send_jwks(int c, const char *body) {
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(body), body);
    char buf[256];
    (void)recv(c, buf, sizeof(buf), 0);
    if (n > 0) (void)send(c, resp, (size_t)n, 0);
    close(c);
}

static void *serve_two(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int c = accept(lfd, NULL, NULL);
    if (c >= 0) send_jwks(c, k1);
    c = accept(lfd, NULL, NULL);
    if (c >= 0) send_jwks(c, k2);
    return NULL;
}

static void *serve_one_then_hang(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int c = accept(lfd, NULL, NULL);
    if (c >= 0) send_jwks(c, k1);
    return NULL;
}

TEST(jwksr, due) {
    ASSERT_EQ(cmq_jwks_refresh_due(0, 0, 0), 0);
    ASSERT_EQ(cmq_jwks_refresh_due(0, 4999, 5), 0);
    ASSERT_EQ(cmq_jwks_refresh_due(0, 5000, 5), 1);
    ASSERT_EQ(cmq_jwks_refresh_due(1000, 5999, 5), 0);
    ASSERT_EQ(cmq_jwks_refresh_due(1000, 6000, 5), 1);
}

TEST(jwksr, step_updates) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_two, (void *)(intptr_t)lfd), 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", port);
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url(url, &u), 0);
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT_NOT_NULL(c);
    uint64_t last = 0;
    ASSERT_EQ(cmq_jwks_refresh_step(&u, c, &last, 0, 5), 0);
    ASSERT_EQ(last, 0);
    ASSERT_EQ(cmq_jwks_refresh_step(&u, c, &last, 5000, 5), 0);
    ASSERT_EQ(last, 5000);
    const cmq_jwks_t *j = cmq_jwks_cache_get(c);
    ASSERT_NOT_NULL(j);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(j, "k1", &sec, &slen), 0);
    ASSERT_EQ(cmq_jwks_refresh_step(&u, c, &last, 10000, 5), 0);
    j = cmq_jwks_cache_get(c);
    ASSERT_EQ(cmq_jwks_lookup(j, "k2", &sec, &slen), 0);
    ASSERT(cmq_jwks_lookup(j, "k1", &sec, &slen) != 0);
    pthread_join(tid, NULL);
    close(lfd);
    cmq_jwks_cache_destroy(c);
}

TEST(jwksr, fail_keeps_old) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_one_then_hang,
                             (void *)(intptr_t)lfd),
              0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", port);
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url(url, &u), 0);
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    uint64_t last = 0;
    ASSERT_EQ(cmq_jwks_refresh_step(&u, c, &last, 5000, 5), 0);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(cmq_jwks_cache_get(c), "k1", &sec, &slen), 0);
    close(lfd);
    pthread_join(tid, NULL);
    ASSERT(cmq_jwks_refresh_step(&u, c, &last, 10000, 5) != 0);
    ASSERT_EQ(cmq_jwks_lookup(cmq_jwks_cache_get(c), "k1", &sec, &slen), 0);
    cmq_jwks_cache_destroy(c);
}

TEST(jwksr, reject) {
    ASSERT(cmq_jwks_refresh_due(0, 1, 0) == 0);
    ASSERT(cmq_jwks_refresh_step(NULL, NULL, NULL, 1, 5) != 0);
    ASSERT(cmq_jwks_cache_put(NULL, NULL) != 0);
    ASSERT(cmq_jwks_cache_get(NULL) == NULL);
    ASSERT(cmq_jwks_refresh_start(NULL, NULL, 5) == NULL);
}

TEST_MAIN()
