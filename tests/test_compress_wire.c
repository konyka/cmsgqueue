/* v0.5.41: F2 BATCH compression on the wire.
 *
 * A high-ratio payload (4 KiB of 'A') proves handle_batch sizes
 * the dest from ZSTD content size, not ZSTD_compressBound(compressed).
 */
#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_compress.h"
#include "cmq_test.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define COMPRESS_WIRE_PORT 19850

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    return fd;
}

static ssize_t send_frame_flags(int fd, cmq_op_t op, uint8_t flags,
                                 const uint8_t *payload, size_t plen) {
    uint8_t buf[16384];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, flags, payload, plen);
    if (len == 0) return -1;
    return write(fd, buf, len);
}

static int recv_frame(int fd, cmq_frame_t *frame, cmq_parser_t *parser) {
    for (int retry = 0; retry < 200; retry++) {
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
        uint8_t buf[8192];
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

static void free_frame(cmq_frame_t *f) {
    free(f->payload);
    f->payload = NULL;
}

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame_flags(fd, CMQ_OP_CONNECT, 0, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    if (recv_frame(fd, &frame, parser) != 0) return;
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame(&frame);
        if (recv_frame(fd, &frame, parser) != 0) return;
    }
    if (frame.hdr.op == CMQ_OP_CONNACK) free_frame(&frame);
}

static int do_subscribe(int fd, cmq_parser_t *parser, const char *subject,
                         uint32_t sub_id) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[256];
    buf[0] = (sub_id >> 24) & 0xFF;
    buf[1] = (sub_id >> 16) & 0xFF;
    buf[2] = (sub_id >> 8) & 0xFF;
    buf[3] = sub_id & 0xFF;
    buf[4] = (slen >> 8) & 0xFF;
    buf[5] = slen & 0xFF;
    memcpy(buf + 6, subject, slen);
    send_frame_flags(fd, CMQ_OP_SUBSCRIBE, 0, buf, 6 + slen);
    wait_ms(50);
    cmq_frame_t f;
    if (recv_frame(fd, &f, parser) != 0) return -1;
    int ok = (f.hdr.op == CMQ_OP_SUBACK);
    free_frame(&f);
    return ok ? 0 : -1;
}

static size_t build_one_msg_batch(uint8_t *out, size_t cap,
                                   const char *subject,
                                   const uint8_t *body, size_t blen) {
    uint16_t slen = (uint16_t)strlen(subject);
    size_t need = 2 + 2 + slen + 2 + 4 + blen;
    if (need > cap) return 0;
    size_t off = 0;
    out[off++] = 0;
    out[off++] = 1;
    out[off++] = (uint8_t)(slen >> 8);
    out[off++] = (uint8_t)(slen & 0xFF);
    memcpy(out + off, subject, slen);
    off += slen;
    out[off++] = 0;
    out[off++] = 0;
    uint32_t plen = (uint32_t)blen;
    out[off++] = (uint8_t)(plen >> 24);
    out[off++] = (uint8_t)(plen >> 16);
    out[off++] = (uint8_t)(plen >> 8);
    out[off++] = (uint8_t)plen;
    memcpy(out + off, body, blen);
    off += blen;
    return off;
}

TEST(compress_wire, parser_accepts_compressed_batch) {
    uint8_t batch[128];
    const uint8_t body[] = "hi";
    size_t raw_n = build_one_msg_batch(batch, sizeof(batch), "zstd.p",
                                        body, sizeof(body) - 1);
    ASSERT(raw_n > 0);
    size_t cap = cmq_compress_bound(raw_n);
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress(batch, raw_n, enc, cap);
    ASSERT(enc_len > 0);

    cmq_parser_t *p = cmq_parser_create();
    uint8_t framed[512];
    size_t n = cmq_frame_encode(framed, sizeof(framed), CMQ_OP_BATCH,
                                CMQ_FLAG_COMPRESSED, enc, (size_t)enc_len);
    ASSERT(n > 0);
    ASSERT_EQ(cmq_parser_feed(p, framed, n), 1);
    ASSERT_EQ(cmq_parser_pending_error(p), 0);
    const cmq_frame_t *f = cmq_parser_frame(p);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->hdr.op, CMQ_OP_BATCH);
    ASSERT(f->hdr.flags & CMQ_FLAG_COMPRESSED);
    cmq_parser_destroy(p);
    free(enc);
}

TEST(compress_wire, batch_delivers_high_ratio) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = COMPRESS_WIRE_PORT;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(COMPRESS_WIRE_PORT);
    ASSERT(sub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "zstd.big", 1), 0);

    int pub_fd = connect_to(COMPRESS_WIRE_PORT);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(50);

    static uint8_t body[4096];
    memset(body, 'A', sizeof(body));
    uint8_t raw[4200];
    size_t raw_n = build_one_msg_batch(raw, sizeof(raw), "zstd.big",
                                        body, sizeof(body));
    ASSERT(raw_n > 0);
    size_t cap = cmq_compress_bound(raw_n);
    uint8_t *enc = malloc(cap);
    ASSERT_NOT_NULL(enc);
    ssize_t enc_len = cmq_compress(raw, raw_n, enc, cap);
    ASSERT(enc_len > 0);
    /* If dest were compress_bound(enc_len), decompress would fail. */
    ASSERT((size_t)cmq_compress_bound((size_t)enc_len) < raw_n);

    ASSERT(send_frame_flags(pub_fd, CMQ_OP_BATCH, CMQ_FLAG_COMPRESSED,
                            enc, (size_t)enc_len) > 0);
    wait_ms(300);

    cmq_frame_t f;
    int got = 0;
    if (recv_frame(sub_fd, &f, sub_parser) == 0 && f.hdr.op == CMQ_OP_MESSAGE) {
        got = 1;
    }
    free_frame(&f);
    ASSERT_EQ(got, 1);

    free(enc);
    close(sub_fd);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    cmq_parser_destroy(pub_parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
