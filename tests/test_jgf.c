/* v0.5.145: reload GETs jwks_url when create had no cache. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
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

TEST(jgf, apply) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, serve_jwks, (void *)(intptr_t)lfd), 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks", port);
    cmq_jwks_cache_t *c = NULL;
    ASSERT_EQ(cmq_jwks_reload_fetch(&c, url, NULL), 0);
    ASSERT(c != NULL);
    const cmq_jwks_t *got = cmq_jwks_cache_get(c);
    ASSERT(got != NULL);
    ASSERT(got->n > 0);
    cmq_jwks_cache_t *same = c;
    ASSERT_EQ(cmq_jwks_reload_fetch(&c, "http://10.0.0.1/jwks", NULL), 0);
    ASSERT(c == same);
    pthread_join(th, NULL);
    close(lfd);
    cmq_jwks_cache_destroy(c);
}

TEST(jgf, omitted) {
    cmq_jwks_cache_t *c = NULL;
    ASSERT_EQ(cmq_jwks_reload_fetch(&c, NULL, NULL), 0);
    ASSERT(c == NULL);
}

TEST(jgf, empty) {
    cmq_jwks_cache_t *c = NULL;
    ASSERT_EQ(cmq_jwks_reload_fetch(&c, "", NULL), 0);
    ASSERT(c == NULL);
}

TEST(jgf, reject) {
    cmq_jwks_cache_t *c = NULL;
    ASSERT(cmq_jwks_reload_fetch(&c, "ftp://127.0.0.1/jwks", NULL) != 0);
    ASSERT(c == NULL);
    ASSERT(cmq_jwks_reload_fetch(NULL, "http://127.0.0.1/jwks", NULL) != 0);
}

TEST_MAIN()
