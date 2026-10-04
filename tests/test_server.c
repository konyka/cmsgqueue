#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_test.h"
#include "cmq_jwt.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <errno.h>

static void b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        if (o + 4 >= cap) break;
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = table[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = table[v & 63];
    }
    out[o] = '\0';
}

static void mint_hs256(char *out, size_t cap, const char *secret,
                       const char *sub) {
    const char *header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char h[128], p[256], signing[400], sig[64];
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"iss\":\"cmq\",\"sub\":\"%s\",\"exp\":4102444800}", sub);
    b64url_encode((const uint8_t *)header, strlen(header), h, sizeof(h));
    b64url_encode((const uint8_t *)payload, strlen(payload), p, sizeof(p));
    snprintf(signing, sizeof(signing), "%s.%s", h, p);
    unsigned char mac[32];
    unsigned int mac_n = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)signing, strlen(signing), mac, &mac_n);
    b64url_encode(mac, mac_n, sig, sizeof(sig));
    snprintf(out, cap, "%s.%s", signing, sig);
}

static void hex_of(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 15];
    }
    out[n * 2] = '\0';
}

static int mint_nkey(uint8_t pub[32], char sig_hex[129], const char *user) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *mctx = NULL;
    uint8_t sig[64];
    size_t pub_len = 32, sig_len = sizeof(sig);
    char msg[320];
    int rc = -1;
    if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
        EVP_PKEY_keygen(kctx, &pkey) != 1 ||
        EVP_PKEY_get_raw_public_key(pkey, pub, &pub_len) != 1)
        goto done;
    snprintf(msg, sizeof(msg), "CMQNK1|%s", user);
    mctx = EVP_MD_CTX_new();
    if (mctx && EVP_DigestSignInit(mctx, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestSign(mctx, sig, &sig_len, (const uint8_t *)msg,
                       strlen(msg)) == 1) {
        hex_of(sig, sizeof(sig), sig_hex);
        rc = 0;
    }
done:
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(kctx);
    return rc;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t send_frame(int fd, cmq_op_t op, const uint8_t *payload, size_t plen) {
    uint8_t buf[4096];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
    if (len == 0) return -1;
    return write(fd, buf, len);
}

static int recv_frame(int fd, cmq_frame_t *frame, cmq_parser_t *parser) {
    for (int retry = 0; retry < 50; retry++) {
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (f) {
            frame->hdr = f->hdr;
            frame->payload_len = f->payload_len;
            if (f->payload_len > 0 && f->payload) {
                frame->payload = malloc(f->payload_len);
                memcpy(frame->payload, f->payload, f->payload_len);
            } else {
                frame->payload = NULL;
            }
            cmq_parser_next(parser);
            return 0;
        }
        uint8_t buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        cmq_parser_feed(parser, buf, (size_t)n);
    }
    return -1;
}

static void free_frame_payload(cmq_frame_t *frame) {
    free(frame->payload);
    frame->payload = NULL;
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);
}

static void send_credentials(int fd, const char *user, const char *pass) {
    uint8_t payload[CMQ_JWT_TOKEN_MAX + 260];
    size_t ulen = strlen(user), plen = strlen(pass);
    ASSERT(ulen < 256 && plen <= CMQ_JWT_TOKEN_MAX);
    payload[0] = (uint8_t)(ulen >> 8);
    payload[1] = (uint8_t)ulen;
    payload[2] = (uint8_t)(plen >> 8);
    payload[3] = (uint8_t)plen;
    memcpy(payload + 4, user, ulen);
    memcpy(payload + 4 + ulen, pass, plen);
    ASSERT(send_frame(fd, CMQ_OP_CONNECT, payload, 4 + ulen + plen) > 0);
}

static uint8_t recv_connack(cmq_parser_t *parser, int fd) {
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    uint8_t code = frame.payload ? frame.payload[0] : 255;
    free_frame_payload(&frame);
    return code;
}

static void *server_thread(void *arg);

static void start_auth_server(cmq_server_t **out, pthread_t *tid,
                              cmq_config_t *config) {
    ASSERT_EQ(cmq_server_create(out, config), CMQ_OK);
    ASSERT_EQ(pthread_create(tid, NULL, server_thread, *out), 0);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
}

TEST(server, connect_jwt_verifies_wire_credentials) {
    char valid[CMQ_JWT_TOKEN_MAX + 1];
    mint_hs256(valid, sizeof(valid), "secret", "alice");
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 28804;
    config.log_to_stdout = 0;
    config.jwt_issuer = "cmq";
    config.jwt_hmac_secret = "secret";
    cmq_server_t *srv = NULL;
    pthread_t tid;
    start_auth_server(&srv, &tid, &config);
    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    send_credentials(fd, "ignored", "not-a-token");
    ASSERT_EQ(recv_connack(parser, fd), 2);
    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);

    start_auth_server(&srv, &tid, &config);
    fd = connect_to(config.port);
    ASSERT(fd >= 0);
    parser = cmq_parser_create();
    send_credentials(fd, "ignored", valid);
    ASSERT_EQ(recv_connack(parser, fd), 0);
    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server, connect_nkey_verifies_wire_signature) {
    uint8_t pub[32];
    char sig[129];
    ASSERT_EQ(mint_nkey(pub, sig, "leaf1"), 0);
    char pub_hex[65];
    hex_of(pub, sizeof(pub), pub_hex);
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 28805;
    config.log_to_stdout = 0;
    config.nkey_pub = pub_hex;
    cmq_server_t *srv = NULL;
    pthread_t tid;
    start_auth_server(&srv, &tid, &config);
    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    sig[0] = sig[0] == '0' ? '1' : '0';
    send_credentials(fd, "leaf1", sig);
    ASSERT_EQ(recv_connack(parser, fd), 2);
    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);

    ASSERT_EQ(mint_nkey(pub, sig, "leaf1"), 0);
    hex_of(pub, sizeof(pub), pub_hex);
    start_auth_server(&srv, &tid, &config);
    fd = connect_to(config.port);
    ASSERT(fd >= 0);
    parser = cmq_parser_create();
    send_credentials(fd, "leaf1", sig);
    ASSERT_EQ(recv_connack(parser, fd), 0);
    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

TEST(server, create_destroy) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    ASSERT_NOT_NULL(srv);
    cmq_server_destroy(srv);
}

TEST(server, bind_accept_info) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 28801;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(28801);
    ASSERT(fd >= 0);

    nanosleep(&ts, NULL);

    cmq_parser_t *parser = cmq_parser_create();
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    nanosleep(&ts, NULL);
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server, connect_pong) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 28802;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int fd = connect_to(28802);
    ASSERT(fd >= 0);
    struct timespec ts2 = {0, 100000000};
    nanosleep(&ts2, NULL);

    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    send_frame(fd, CMQ_OP_PING, NULL, 0);
    ts.tv_sec = 0; ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);

    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_PONG);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server, pubsub_basic) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 28803;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);

    int sub_fd = connect_to(28803);
    ASSERT(sub_fd >= 0);
    struct timespec ts2 = {0, 100000000};
    nanosleep(&ts2, NULL);
    cmq_parser_t *sub_parser = cmq_parser_create();

    do_connect(sub_fd, sub_parser);

    const char *subject = "test.topic";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    uint32_t sub_id = 42;
    sub_pl[0] = (sub_id >> 24) & 0xFF;
    sub_pl[1] = (sub_id >> 16) & 0xFF;
    sub_pl[2] = (sub_id >> 8) & 0xFF;
    sub_pl[3] = sub_id & 0xFF;
    sub_pl[4] = (slen >> 8) & 0xFF;
    sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);

    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    ts.tv_sec = 0; ts.tv_nsec = 50000000;
    nanosleep(&ts, NULL);

    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_SUBACK);
    ASSERT_EQ(frame.payload_len, (size_t)5);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);

    int pub_fd = connect_to(28803);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pub_parser = cmq_parser_create();

    do_connect(pub_fd, pub_parser);

    const char *msg = "hello";
    size_t msg_len = strlen(msg);
    uint8_t pub_pl[256];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0;
    pub_pl[off++] = 0;
    memcpy(pub_pl + off, msg, msg_len);
    off += msg_len;

    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
    ts.tv_sec = 0; ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sub_parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_MESSAGE);
    free_frame_payload(&frame);

    cmq_parser_destroy(pub_parser);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
