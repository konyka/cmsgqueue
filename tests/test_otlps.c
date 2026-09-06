/* v0.5.78: D1 OTLP HTTPS POST. */
#include "cmq_test.h"
#include "cmq_otlp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OTLPS_DIR "/tmp/cmq-otlps"

static void gen_cert(void) {
    (void)system("rm -rf " OTLPS_DIR " && mkdir -p " OTLPS_DIR);
    (void)system("openssl req -x509 -newkey rsa:2048 -keyout "
                 OTLPS_DIR "/key.pem -out " OTLPS_DIR "/cert.pem "
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

static void *serve_otlp_tls(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return NULL;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        close(c);
        return NULL;
    }
    if (SSL_CTX_use_certificate_file(ctx, OTLPS_DIR "/cert.pem",
                                     SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, OTLPS_DIR "/key.pem",
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
        char buf[512];
        (void)SSL_read(ssl, buf, sizeof(buf));
        const char *resp =
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)SSL_write(ssl, resp, (int)strlen(resp));
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(c);
    return NULL;
}

TEST(otlps, parse_https) {
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url("https://127.0.0.1:4318/v1/traces", &u), 0);
    ASSERT_EQ(u.tls, 1);
    ASSERT_EQ(u.port, 4318);
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT_STR_EQ(u.path, "/v1/traces");
    ASSERT_EQ(cmq_otlp_parse_url("https://collector.local", &u), 0);
    ASSERT_EQ(u.tls, 1);
    ASSERT_EQ(u.port, CMQ_OTLP_DEFAULT_PORT);
    ASSERT_STR_EQ(u.path, "/v1/traces");
}

TEST(otlps, https_post) {
    gen_cert();
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_otlp_tls, (void *)(intptr_t)lfd),
              0);
    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/v1/traces", port);
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url(url, &u), 0);
    ASSERT_EQ(cmq_otlp_set_ca(&u, OTLPS_DIR "/cert.pem"), 0);
    ASSERT_EQ(cmq_otlp_http_post(&u, "{\"n\":1}"), 0);
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(otlps, no_ca_fails) {
    gen_cert();
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, serve_otlp_tls, (void *)(intptr_t)lfd),
              0);
    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/v1/traces", port);
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url(url, &u), 0);
    ASSERT(cmq_otlp_http_post(&u, "{\"n\":1}") != 0);
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(otlps, reject) {
    cmq_otlp_url_t u;
    ASSERT(cmq_otlp_parse_url("https://user:pass@127.0.0.1/x", &u) != 0);
    ASSERT(cmq_otlp_parse_url("https://127.0.0.1/../etc", &u) != 0);
    ASSERT(cmq_otlp_set_ca(NULL, OTLPS_DIR "/cert.pem") != 0);
    ASSERT(cmq_otlp_set_ca(&u, "../cert.pem") != 0);
    ASSERT(cmq_otlp_http_post(NULL, "{}") != 0);
}

TEST_MAIN()
