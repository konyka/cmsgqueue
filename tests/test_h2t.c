/* v0.5.83: D2 TLS-wrapped h2 I/O. */
#include "cmq_test.h"
#include "cmq_h2.h"
#include "cmq_hpack.h"
#include "cmq_tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define H2T_DIR "/tmp/cmq-h2t"

static void put_hdr(uint8_t *b, uint32_t len, uint8_t type, uint8_t flags,
                    uint32_t sid) {
    b[0] = (uint8_t)((len >> 16) & 0xff);
    b[1] = (uint8_t)((len >> 8) & 0xff);
    b[2] = (uint8_t)(len & 0xff);
    b[3] = type;
    b[4] = flags;
    b[5] = (uint8_t)((sid >> 24) & 0x7f);
    b[6] = (uint8_t)((sid >> 16) & 0xff);
    b[7] = (uint8_t)((sid >> 8) & 0xff);
    b[8] = (uint8_t)(sid & 0xff);
}

static int build_post(uint8_t *out, size_t cap, const char *path,
                      const uint8_t *body, size_t blen) {
    uint8_t blk[128];
    size_t bo = 0;
    blk[bo++] = 0x83;
    int n = cmq_hpack_int_encode(4, 6, 0x40, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    n = cmq_hpack_str_encode(path, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    if (9 + bo + 9 + blen > cap) return -1;
    size_t o = 0;
    memcpy(out + o, cmq_h2_preface, CMQ_H2_PREFACE_LEN);
    o += CMQ_H2_PREFACE_LEN;
    uint8_t setpl[6];
    if (cmq_h2_settings_encode(32, setpl, sizeof(setpl)) != 6) return -1;
    put_hdr(out + o, 6, CMQ_H2_TYPE_SETTINGS, 0, 0);
    memcpy(out + o + 9, setpl, 6);
    o += 15;
    put_hdr(out + o, (uint32_t)bo, CMQ_H2_TYPE_HEADERS, 0x04, 1);
    memcpy(out + o + 9, blk, bo);
    o += 9 + bo;
    put_hdr(out + o, (uint32_t)blen, CMQ_H2_TYPE_DATA, 0x01, 1);
    memcpy(out + o + 9, body, blen);
    o += 9 + blen;
    return (int)o;
}

static void gen_cert(void) {
    (void)system("rm -rf " H2T_DIR " && mkdir -p " H2T_DIR);
    (void)system("openssl req -x509 -newkey rsa:2048 -keyout "
                 H2T_DIR "/key.pem -out " H2T_DIR "/cert.pem "
                 "-days 1 -nodes -subj '/CN=127.0.0.1' "
                 "-addext 'subjectAltName=IP:127.0.0.1' 2>/dev/null");
}

static int client_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct h2t_cli {
    int port;
    const uint8_t *req;
    int n;
    int use_tls;
    int rc;
    atomic_int done;
};

static void h2t_wait_done(struct h2t_cli *c) {
    for (int i = 0; i < 50 && !atomic_load(&c->done); i++) {
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
    }
}

static int tls_client_write(int fd, const uint8_t *p, size_t n,
                            struct h2t_cli *hold) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_load_verify_locations(ctx, H2T_DIR "/cert.pem", NULL) != 1) {
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_set_fd(ssl, fd);
    X509_VERIFY_PARAM *pm = SSL_get0_param(ssl);
    if (pm) (void)X509_VERIFY_PARAM_set1_ip_asc(pm, "127.0.0.1");
    int rc = -1;
    if (SSL_connect(ssl) == 1) {
        size_t off = 0;
        int ok = 1;
        while (off < n) {
            int w = SSL_write(ssl, p + off, (int)(n - off));
            if (w <= 0) {
                ok = 0;
                break;
            }
            off += (size_t)w;
        }
        if (ok) rc = 0;
    }
    if (hold) h2t_wait_done(hold);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return rc;
}

static void *client_thr(void *arg) {
    struct h2t_cli *c = arg;
    int fd = client_connect(c->port);
    if (fd < 0) {
        c->rc = -1;
        return NULL;
    }
    if (c->use_tls)
        c->rc = tls_client_write(fd, c->req, (size_t)c->n, c);
    else {
        size_t off = 0;
        c->rc = 0;
        while (off < (size_t)c->n) {
            ssize_t w = send(fd, c->req + off, (size_t)c->n - off, 0);
            if (w <= 0) {
                c->rc = -1;
                break;
            }
            off += (size_t)w;
        }
    }
    h2t_wait_done(c);
    close(fd);
    return NULL;
}

static cmq_tls_config_t *load_srv_tls(void) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    if (!cfg) return NULL;
    if (cmq_tls_set_cert(cfg, H2T_DIR "/cert.pem") != 0 ||
        cmq_tls_set_key(cfg, H2T_DIR "/key.pem") != 0 ||
        cmq_tls_set_alpn(cfg, "h2") != 0 ||
        cmq_tls_load(cfg) != 0) {
        cmq_tls_config_destroy(cfg);
        return NULL;
    }
    return cfg;
}

TEST(h2t, tls_post) {
    gen_cert();
    int lfd = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(lfd >= 0);
    int port = cmq_h2_listen_port(lfd);
    uint8_t req[256];
    int n = build_post(req, sizeof(req), "/foo.bar", (const uint8_t *)"hi", 2);
    ASSERT(n > 0);
    struct h2t_cli cli = {.port = port, .req = req, .n = n, .use_tls = 1};
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, client_thr, &cli), 0);
    cmq_tls_config_t *cfg = load_srv_tls();
    ASSERT_NOT_NULL(cfg);
    char sub[32];
    uint8_t body[16];
    size_t blen = 0;
    ASSERT_EQ(cmq_h2_accept_tls(lfd, cfg, sub, sizeof(sub), body,
                                sizeof(body), &blen),
              0);
    atomic_store(&cli.done, 1);
    ASSERT_STR_EQ(sub, "foo.bar");
    ASSERT_EQ(blen, 2u);
    ASSERT(memcmp(body, "hi", 2) == 0);
    pthread_join(tid, NULL);
    ASSERT_EQ(cli.rc, 0);
    cmq_tls_config_destroy(cfg);
    close(lfd);
}

TEST(h2t, plain_fail) {
    gen_cert();
    int lfd = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(lfd >= 0);
    uint8_t req[256];
    int n = build_post(req, sizeof(req), "/foo.bar", (const uint8_t *)"hi", 2);
    ASSERT(n > 0);
    struct h2t_cli cli = {
        .port = cmq_h2_listen_port(lfd), .req = req, .n = n, .use_tls = 0};
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, client_thr, &cli), 0);
    cmq_tls_config_t *cfg = load_srv_tls();
    ASSERT_NOT_NULL(cfg);
    char sub[8];
    uint8_t body[8];
    size_t blen = 0;
    ASSERT(cmq_h2_accept_tls(lfd, cfg, sub, sizeof(sub), body, sizeof(body),
                             &blen) != 0);
    atomic_store(&cli.done, 1);
    pthread_join(tid, NULL);
    cmq_tls_config_destroy(cfg);
    close(lfd);
}

TEST(h2t, listen_plain) {
    int lfd = cmq_h2_listen(NULL, 0);
    ASSERT(lfd >= 0);
    uint8_t req[256];
    int n = build_post(req, sizeof(req), "/foo.bar", (const uint8_t *)"hi", 2);
    ASSERT(n > 0);
    struct h2t_cli cli = {
        .port = cmq_h2_listen_port(lfd), .req = req, .n = n, .use_tls = 0};
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, client_thr, &cli), 0);
    char sub[32];
    uint8_t body[16];
    size_t blen = 0;
    ASSERT_EQ(cmq_h2_accept(lfd, sub, sizeof(sub), body, sizeof(body), &blen),
              0);
    atomic_store(&cli.done, 1);
    ASSERT_STR_EQ(sub, "foo.bar");
    pthread_join(tid, NULL);
    close(lfd);
}

TEST(h2t, reject) {
    char sub[8];
    uint8_t body[8];
    size_t blen = 0;
    ASSERT(cmq_h2_accept_tls(-1, NULL, sub, sizeof(sub), body, sizeof(body),
                             &blen) != 0);
    ASSERT(cmq_h2_session_tls(NULL, sub, sizeof(sub), body, sizeof(body),
                              &blen) != 0);
}

TEST_MAIN()
