/* v0.5.76: D3 remote JWKS HTTP GET. */
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

static const char *k_jwks =
    "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"k\":\"czNjcmV0\"}]}";

static void *serve_jwks(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return NULL;
    char buf[256];
    (void)recv(c, buf, sizeof(buf), 0);
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(k_jwks), k_jwks);
    if (n > 0) (void)send(c, resp, (size_t)n, 0);
    close(c);
    return NULL;
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

TEST(jwksf, parse_url) {
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url("http://127.0.0.1:8080/jwks.json", &u), 0);
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT_EQ(u.port, 8080);
    ASSERT_STR_EQ(u.path, "/jwks.json");
    ASSERT_EQ(cmq_jwks_parse_url("http://keys.local", &u), 0);
    ASSERT_EQ(u.port, CMQ_JWKS_DEFAULT_PORT);
    ASSERT_STR_EQ(u.path, "/.well-known/jwks.json");
}

TEST(jwksf, build_get) {
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url("http://127.0.0.1:80/jwks.json", &u), 0);
    char req[256];
    ASSERT(cmq_jwks_build_get(&u, req, sizeof(req)) > 0);
    ASSERT(strstr(req, "GET /jwks.json HTTP/1.1") != NULL);
    ASSERT(strstr(req, "Host: 127.0.0.1:80") != NULL);
    ASSERT(strstr(req, "\r\n\r\n") != NULL);
}

TEST(jwksf, http_get) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_jwks, (void *)(intptr_t)lfd), 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", port);
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url(url, &u), 0);
    cmq_jwks_t j;
    ASSERT_EQ(cmq_jwks_http_get(&u, &j), 0);
    ASSERT_EQ(j.n, 1);
    const uint8_t *sec = NULL;
    size_t slen = 0;
    ASSERT_EQ(cmq_jwks_lookup(&j, "k1", &sec, &slen), 0);
    ASSERT_EQ(slen, 6u);
    ASSERT(memcmp(sec, "s3cret", 6) == 0);
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(jwksf, reject) {
    cmq_jwks_url_t u;
    ASSERT(cmq_jwks_parse_url(NULL, &u) != 0);
    ASSERT(cmq_jwks_parse_url("https://127.0.0.1/jwks.json", &u) != 0);
    ASSERT(cmq_jwks_parse_url("http://user:pass@127.0.0.1/x", &u) != 0);
    ASSERT(cmq_jwks_parse_url("http://127.0.0.1/../jwks", &u) != 0);
    ASSERT(cmq_jwks_http_get(NULL, NULL) != 0);
}

TEST_MAIN()
