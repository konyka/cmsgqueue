/* F2: End-to-end wire test for compressed BATCH.
 *
 * Drives the full path:
 *   client -> zstd-compressed BATCH frame with CMQ_FLAG_COMPRESSED ->
 *   server's handle_batch decompresses -> dispatches individual publishes ->
 *   subscriber receives MESSAGE frames containing the original payloads.
 *
 * This test runs against a real listener, not a parser unit. It is the
 * regression gate for F2: it must fail on master (parser rejects the
 * flag, server tears down the connection) and pass once F2 ships
 * (parser accepts BATCH+COMPRESSED, handle_batch decompresses).
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

#define COMPRESSED_PORT 19810

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

static ssize_t send_frame(int fd, cmq_op_t op, uint8_t flags,
                          const uint8_t *payload, size_t plen) {
    uint8_t buf[8192];
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
            } else { frame->payload = NULL; }
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
    send_frame(fd, CMQ_OP_CONNECT, 0, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    if (recv_frame(fd, &frame, parser) != 0) return;
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame(&frame);
        if (recv_frame(fd, &frame, parser) != 0) return;
    }
    if (frame.hdr.op == CMQ_OP_CONNACK) free_frame(&frame);
}

static int do_subscribe(int fd, cmq_parser_t *parser,
                        const char *subject, uint32_t sub_id) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[256];
    buf[0] = (sub_id >> 24) & 0xFF;
    buf[1] = (sub_id >> 16) & 0xFF;
    buf[2] = (sub_id >> 8) & 0xFF;
    buf[3] = sub_id & 0xFF;
    buf[4] = (slen >> 8) & 0xFF;
    buf[5] = slen & 0xFF;
    memcpy(buf + 6, subject, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, 0, buf, 6 + slen);
    wait_ms(50);
    cmq_frame_t f;
    if (recv_frame(fd, &f, parser) != 0) return -1;
    int ok = (f.hdr.op == CMQ_OP_SUBACK);
    free_frame(&f);
    return ok ? 0 : -1;
}

/* Build the wire payload of an uncompressed BATCH frame containing two
 * entries. Returns payload length on success, 0 on overflow. The layout
 * matches the format documented in src/server/cmq_server.c handle_batch. */
static size_t build_uncompressed_batch(uint8_t *out, size_t cap) {
    const char *sub1 = "compressed.a";
    const char *sub2 = "compressed.b";
    const char *msg1 = "hello-compressed-a";
    const char *msg2 = "hello-compressed-b";
    uint16_t slen1 = (uint16_t)strlen(sub1);
    uint16_t slen2 = (uint16_t)strlen(sub2);
    uint32_t plen1 = (uint32_t)strlen(msg1);
    uint32_t plen2 = (uint32_t)strlen(msg2);
    size_t need = 2 + (2 + slen1 + 2 + 4 + plen1) + (2 + slen2 + 2 + 4 + plen2);
    if (need > cap) return 0;
    size_t off = 0;
    out[off++] = 0; out[off++] = 2;
    out[off++] = (slen1 >> 8) & 0xFF; out[off++] = slen1 & 0xFF;
    memcpy(out + off, sub1, slen1); off += slen1;
    out[off++] = 0; out[off++] = 0;
    out[off++] = (plen1 >> 24) & 0xFF; out[off++] = (plen1 >> 16) & 0xFF;
    out[off++] = (plen1 >> 8) & 0xFF; out[off++] = plen1 & 0xFF;
    memcpy(out + off, msg1, plen1); off += plen1;
    out[off++] = (slen2 >> 8) & 0xFF; out[off++] = slen2 & 0xFF;
    memcpy(out + off, sub2, slen2); off += slen2;
    out[off++] = 0; out[off++] = 0;
    out[off++] = (plen2 >> 24) & 0xFF; out[off++] = (plen2 >> 16) & 0xFF;
    out[off++] = (plen2 >> 8) & 0xFF; out[off++] = plen2 & 0xFF;
    memcpy(out + off, msg2, plen2); off += plen2;
    return off;
}

/* Extract the subject and payload body from a MESSAGE frame and compare
 * to expected. Returns 0 on match, -1 otherwise.
 *
 * MESSAGE payload layout (cmq_build_message_frame in cmq_server.c):
 *   sub_id (4 bytes BE)
 *   subject_len (2 bytes BE)
 *   subject (subject_len bytes)
 *   headers_len (2 bytes BE)   [may be 0]
 *   headers (headers_len bytes, may be absent if CMQ_FLAG_HEADERS unset)
 *   payload_len (4 bytes BE)
 *   payload (payload_len bytes)
 */
static int check_message_payload(const cmq_frame_t *f,
                                 const char *expect_subject,
                                 const char *expect_body) {
    if (f->hdr.op != CMQ_OP_MESSAGE) return -1;
    if (f->payload_len < 4u + 2u) return -1;
    size_t off = 0;
    off += 4;
    uint16_t slen = ((uint16_t)f->payload[off] << 8) | (uint16_t)f->payload[off + 1];
    off += 2;
    if (f->payload_len < off + (size_t)slen + 2u + 4u) return -1;
    if (slen != strlen(expect_subject)) return -1;
    if (memcmp(f->payload + off, expect_subject, slen) != 0) return -1;
    off += slen;
    uint16_t hlen = ((uint16_t)f->payload[off] << 8) | (uint16_t)f->payload[off + 1];
    off += 2;
    if (f->payload_len < off + (size_t)hlen + 4u) return -1;
    off += hlen;
    uint32_t plen = ((uint32_t)f->payload[off] << 24)
                  | ((uint32_t)f->payload[off + 1] << 16)
                  | ((uint32_t)f->payload[off + 2] << 8)
                  |  (uint32_t)f->payload[off + 3];
    off += 4;
    if (f->payload_len < off + plen) return -1;
    size_t body_len = strlen(expect_body);
    if (plen != body_len) return -1;
    if (memcmp(f->payload + off, expect_body, body_len) != 0) return -1;
    return 0;
}

/* F2: Compressed BATCH end-to-end. Client compresses a two-message
 * batch with zstd, sets CMQ_FLAG_COMPRESSED, and sends it. Server's
 * handle_batch decompresses and dispatches. The subscriber should
 * receive both messages byte-exact. */
TEST(compressed_batch, wire_round_trip) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = COMPRESSED_PORT;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(COMPRESSED_PORT);
    ASSERT(sub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "compressed.a", 1), 0);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "compressed.b", 2), 0);

    int pub_fd = connect_to(COMPRESSED_PORT);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(100);

    /* Build an uncompressed BATCH payload, then zstd-compress it. */
    uint8_t batch[512];
    size_t batch_len = build_uncompressed_batch(batch, sizeof(batch));
    ASSERT(batch_len > 0);

    size_t cap = cmq_compress_bound(batch_len);
    ASSERT(cap <= sizeof(batch));
    uint8_t compressed[512];
    ssize_t enc_len = cmq_compress(batch, batch_len, compressed, cap);
    ASSERT(enc_len > 0);

    ssize_t w = send_frame(pub_fd, CMQ_OP_BATCH, CMQ_FLAG_COMPRESSED,
                           compressed, (size_t)enc_len);
    ASSERT(w > 0);
    wait_ms(400);

    /* Receive up to two MESSAGE frames on the subscriber side. The
     * server should have decompressed the payload and dispatched both
     * entries as if they had been sent uncompressed. */
    int got_a = 0, got_b = 0;
    for (int attempt = 0; attempt < 6 && (got_a == 0 || got_b == 0); attempt++) {
        cmq_frame_t f;
        if (recv_frame(sub_fd, &f, sub_parser) != 0) break;
        if (f.hdr.op == CMQ_OP_MESSAGE) {
            if (check_message_payload(&f, "compressed.a",
                                       "hello-compressed-a") == 0) {
                got_a = 1;
            } else if (check_message_payload(&f, "compressed.b",
                                              "hello-compressed-b") == 0) {
                got_b = 1;
            }
        }
        free_frame(&f);
    }
    ASSERT_EQ(got_a, 1);
    ASSERT_EQ(got_b, 1);

    close(sub_fd);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    cmq_parser_destroy(pub_parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* F2: Corrupt zstd payload must surface as a server-side error and
 * must NOT tear down the connection prematurely. Server tears down
 * only on protocol-level errors; corrupt compression is a recoverable
 * payload error. (Behavior matches cmq_send_error in handle_batch.) */
TEST(compressed_batch, corrupt_zstd_payload) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = COMPRESSED_PORT + 1;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int pub_fd = connect_to(COMPRESSED_PORT + 1);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(100);

    /* Send BATCH+COMPRESSED with garbage payload. Server should reject
     * with an ERROR frame, not tear down the connection. */
    uint8_t garbage[64];
    for (size_t i = 0; i < sizeof(garbage); i++) garbage[i] = (uint8_t)i;
    ssize_t w = send_frame(pub_fd, CMQ_OP_BATCH, CMQ_FLAG_COMPRESSED,
                           garbage, sizeof(garbage));
    ASSERT(w > 0);
    wait_ms(200);

    /* The server should send an ERROR frame (op = CMQ_OP_ERROR). */
    int got_error = 0;
    for (int attempt = 0; attempt < 5 && !got_error; attempt++) {
        cmq_frame_t f;
        if (recv_frame(pub_fd, &f, pub_parser) != 0) break;
        if (f.hdr.op == CMQ_OP_ERROR) got_error = 1;
        free_frame(&f);
    }
    ASSERT_EQ(got_error, 1);

    close(pub_fd);
    cmq_parser_destroy(pub_parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()