/* v0.5.79: D3 HTTPS JWKS GET. */
#include "cmq_test.h"
#include "cmq_jwksf.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define JWKSS_DIR "/tmp/cmq-jwkss"

static const char *k_jwks =
    "{\"keys\":[{\"kty\":\"oct\",\"kid\":\"k1\",\"k\":\"czNjcmV0\"}]}";

static void gen_cert(void) {
    (void)system("rm -rf " JWKSS_DIR " && mkdir -p " JWKSS_DIR);
    (void)system("openssl req -x509 -newkey rsa:2048 -keyout "
                 JWKSS_DIR "/key.pem -out " JWKSS_DIR "/cert.pem "
                 "-days 1 -nodes -subj '/CN=127.0.0.1' "
                 "-addext 'subjectAltName=IP:127.0.0.1' 2>/dev/null");
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

static void *serve_jwks_tls(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return NULL;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        close(c);
        return NULL;
    }
    if (SSL_CTX_use_certificate_file(ctx, JWKSS_DIR "/cert.pem",
                                     SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, JWKSS_DIR "/key.pem",
                                    SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        close(c);
        return NULL;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(c);
        return NULL;
    }
    SSL_set_fd(ssl, c);
    if (SSL_accept(ssl) == 1) {
        char buf[256];
        (void)SSL_read(ssl, buf, sizeof(buf));
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n%s",
                         strlen(k_jwks), k_jwks);
        if (n > 0) (void)SSL_write(ssl, resp, n);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(c);
    return NULL;
}

TEST(jwkss, parse_https) {
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url("https://127.0.0.1:443/jwks.json", &u), 0);
    ASSERT_EQ(u.tls, 1);
    ASSERT_EQ(u.port, 443);
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT_STR_EQ(u.path, "/jwks.json");
    ASSERT_EQ(cmq_jwks_parse_url("https://keys.local", &u), 0);
    ASSERT_EQ(u.tls, 1);
    ASSERT_EQ(u.port, CMQ_JWKS_TLS_PORT);
    ASSERT_STR_EQ(u.path, "/.well-known/jwks.json");
}

TEST(jwkss, https_get) {
    gen_cert();
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_jwks_tls, (void *)(intptr_t)lfd),
              0);
    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/jwks.json", port);
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url(url, &u), 0);
    ASSERT_EQ(cmq_jwks_set_ca(&u, JWKSS_DIR "/cert.pem"), 0);
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

TEST(jwkss, no_ca_fails) {
    gen_cert();
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_jwks_tls, (void *)(intptr_t)lfd),
              0);
    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/jwks.json", port);
    cmq_jwks_url_t u;
    ASSERT_EQ(cmq_jwks_parse_url(url, &u), 0);
    cmq_jwks_t j;
    ASSERT(cmq_jwks_http_get(&u, &j) != 0);
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(jwkss, reject) {
    cmq_jwks_url_t u;
    ASSERT(cmq_jwks_parse_url("https://user:pass@127.0.0.1/x", &u) != 0);
    ASSERT(cmq_jwks_parse_url("https://127.0.0.1/../jwks", &u) != 0);
    ASSERT(cmq_jwks_set_ca(NULL, JWKSS_DIR "/cert.pem") != 0);
    ASSERT(cmq_jwks_set_ca(&u, "../cert.pem") != 0);
}

TEST_MAIN()
