#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_ws.h"
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

#define STATS_PORT 19700

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

static ssize_t send_frame(int fd, cmq_op_t op, const uint8_t *payload, size_t plen) {
    uint8_t buf[8192];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
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
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    if (recv_frame(fd, &frame, parser) != 0) return;
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame(&frame);
        if (recv_frame(fd, &frame, parser) != 0) return;
    }
    if (frame.hdr.op == CMQ_OP_CONNACK) free_frame(&frame);
}

static int do_subscribe(int fd, cmq_parser_t *parser, const char *subject, uint32_t sub_id) {
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[256];
    buf[0] = (sub_id >> 24) & 0xFF;
    buf[1] = (sub_id >> 16) & 0xFF;
    buf[2] = (sub_id >> 8) & 0xFF;
    buf[3] = sub_id & 0xFF;
    buf[4] = (slen >> 8) & 0xFF;
    buf[5] = slen & 0xFF;
    memcpy(buf + 6, subject, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, buf, 6 + slen);
    wait_ms(50);
    cmq_frame_t f;
    if (recv_frame(fd, &f, parser) != 0) return -1;
    int ok = (f.hdr.op == CMQ_OP_SUBACK);
    free_frame(&f);
    return ok ? 0 : -1;
}

static void ws_send_cmq(int fd, cmq_op_t op, const uint8_t *payload, size_t plen) {
    uint8_t cmq[8192];
    size_t cmq_len = cmq_frame_encode(cmq, sizeof(cmq), op, 0, payload, plen);
    if (cmq_len == 0) return;
    static const uint8_t mk[4] = {0x37, 0xFA, 0x21, 0x3D};
    size_t hdr = (cmq_len <= 125) ? 2 : (cmq_len <= 65535) ? 4 : 10;
    uint8_t *buf = malloc(hdr + 4 + cmq_len);
    buf[0] = 0x82;
    if (cmq_len <= 125) {
        buf[1] = (uint8_t)(0x80 | cmq_len);
    } else if (cmq_len <= 65535) {
        buf[1] = 0x80 | 126;
        buf[2] = (uint8_t)(cmq_len >> 8);
        buf[3] = (uint8_t)cmq_len;
    } else {
        buf[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) buf[2 + i] = (uint8_t)(cmq_len >> (56 - i * 8));
    }
    buf[hdr] = mk[0]; buf[hdr + 1] = mk[1]; buf[hdr + 2] = mk[2]; buf[hdr + 3] = mk[3];
    for (size_t i = 0; i < cmq_len; i++) buf[hdr + 4 + i] = cmq[i] ^ mk[i % 4];
    write(fd, buf, hdr + 4 + cmq_len);
    free(buf);
}

static int ws_recv_cmq(int fd, cmq_parser_t *parser, cmq_frame_t *out) {
    for (int retry = 0; retry < 200; retry++) {
        const cmq_frame_t *f = cmq_parser_frame(parser);
        if (f) {
            out->hdr = f->hdr; out->payload_len = f->payload_len;
            if (f->payload_len > 0 && f->payload) {
                out->payload = malloc(f->payload_len);
                memcpy(out->payload, f->payload, f->payload_len);
            } else { out->payload = NULL; }
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
        cmq_ws_frame_t wf;
        int parsed = cmq_ws_frame_parse(buf, (size_t)n, &wf);
        if (parsed <= 0 || wf.opcode != CMQ_WS_OPCODE_BINARY || wf.payload_len == 0)
            return -1;
        if (cmq_parser_feed(parser, wf.payload, wf.payload_len) < 0) return -1;
    }
    return -1;
}

TEST(server_ops, stats_query) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    do_subscribe(fd, parser, "stats.test", 1);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(100);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    ASSERT(f.payload_len >= 76);

    uint64_t conn = 0, msg_in = 0, msg_out = 0;
    for (int b = 0; b < 8; b++) conn = (conn << 8) | f.payload[b];
    for (int b = 0; b < 8; b++) msg_in = (msg_in << 8) | f.payload[8 + b];
    for (int b = 0; b < 8; b++) msg_out = (msg_out << 8) | f.payload[16 + b];

    ASSERT(conn >= 1);
    free_frame(&f);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, batch_publish) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 1;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int sub_fd = connect_to(STATS_PORT + 1);
    ASSERT(sub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *sub_parser = cmq_parser_create();
    do_connect(sub_fd, sub_parser);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "batch.a", 1), 0);
    ASSERT_EQ(do_subscribe(sub_fd, sub_parser, "batch.b", 2), 0);

    int pub_fd = connect_to(STATS_PORT + 1);
    ASSERT(pub_fd >= 0);
    wait_ms(20);
    cmq_parser_t *pub_parser = cmq_parser_create();
    do_connect(pub_fd, pub_parser);
    wait_ms(100);

    const char *msg1 = "hello-a";
    const char *msg2 = "hello-b";
    uint16_t slen1 = (uint16_t)strlen("batch.a");
    uint16_t slen2 = (uint16_t)strlen("batch.b");
    size_t mlen1 = strlen(msg1);
    size_t mlen2 = strlen(msg2);

    uint8_t batch[512];
    size_t off = 0;
    batch[off++] = 0;
    batch[off++] = 2;

    batch[off++] = (slen1 >> 8) & 0xFF;
    batch[off++] = slen1 & 0xFF;
    memcpy(batch + off, "batch.a", slen1);
    off += slen1;
    batch[off++] = 0; batch[off++] = 0;
    uint32_t plen1 = (uint32_t)mlen1;
    batch[off++] = (plen1 >> 24) & 0xFF;
    batch[off++] = (plen1 >> 16) & 0xFF;
    batch[off++] = (plen1 >> 8) & 0xFF;
    batch[off++] = plen1 & 0xFF;
    memcpy(batch + off, msg1, mlen1);
    off += mlen1;

    batch[off++] = (slen2 >> 8) & 0xFF;
    batch[off++] = slen2 & 0xFF;
    memcpy(batch + off, "batch.b", slen2);
    off += slen2;
    batch[off++] = 0; batch[off++] = 0;
    uint32_t plen2 = (uint32_t)mlen2;
    batch[off++] = (plen2 >> 24) & 0xFF;
    batch[off++] = (plen2 >> 16) & 0xFF;
    batch[off++] = (plen2 >> 8) & 0xFF;
    batch[off++] = plen2 & 0xFF;
    memcpy(batch + off, msg2, mlen2);
    off += mlen2;

    send_frame(pub_fd, CMQ_OP_BATCH, batch, off);
    wait_ms(300);

    int received = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        cmq_frame_t f;
        if (recv_frame(sub_fd, &f, sub_parser) == 0 && f.hdr.op == CMQ_OP_MESSAGE) {
            received++;
        }
        free_frame(&f);
    }
    ASSERT_EQ(received, 2);

    close(sub_fd);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    cmq_parser_destroy(pub_parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, keepalive_disconnect) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 2;
    config.log_to_stdout = 0;
    config.ping_interval_ms = 500;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT + 2);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);
    wait_ms(100);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(100);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    free_frame(&f);

    wait_ms(1500);

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    ssize_t written = write(fd, "x", 1);
    (void)written;

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, payload_too_large) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 3;
    config.log_to_stdout = 0;
    config.max_payload_size = 64;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT + 3);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    /* PUBLISH frame: [2B subject_len][subject][2B reply_len=0][payload] */
    const char *subject = "cap.test";
    uint16_t slen = (uint16_t)strlen(subject);
    size_t big_len = 128; /* exceeds the 64-byte cap */
    uint8_t buf[512];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subject, slen); off += slen;
    buf[off++] = 0; buf[off++] = 0; /* reply_len = 0 */
    memset(buf + off, 'x', big_len); off += big_len;

    send_frame(fd, CMQ_OP_PUBLISH, buf, off);
    wait_ms(100);

    cmq_frame_t f;
    int got_error = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (recv_frame(fd, &f, parser) != 0) break;
        if (f.hdr.op == CMQ_OP_ERROR) { got_error = 1; free_frame(&f); break; }
        free_frame(&f);
    }
    ASSERT(got_error);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, stats_reject_counters) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 5;
    config.log_to_stdout = 0;
    config.max_payload_size = 64;
    config.max_subs_per_client = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT + 5);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    /* First subscribe succeeds (sub_count 0 -> 1). */
    ASSERT_EQ(do_subscribe(fd, parser, "rej.s1", 1), 0);
    /* Second subscribe rejected: SUBACK code=1 (sub_count already at cap). */
    do_subscribe(fd, parser, "rej.s2", 2);

    /* Now trigger a publish rejection: 128-byte body > 64-byte cap. */
    const char *subject = "rej.test";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t pbuf[512];
    size_t poff = 0;
    pbuf[poff++] = (slen >> 8) & 0xFF;
    pbuf[poff++] = slen & 0xFF;
    memcpy(pbuf + poff, subject, slen); poff += slen;
    pbuf[poff++] = 0; pbuf[poff++] = 0; /* reply_len = 0 */
    memset(pbuf + poff, 'x', 128); poff += 128;
    send_frame(fd, CMQ_OP_PUBLISH, pbuf, poff);

    wait_ms(150);
    /* Drain pending ERROR/SUBACK frames before STATS. */
    for (int i = 0; i < 4; i++) {
        cmq_frame_t junk;
        if (recv_frame(fd, &junk, parser) != 0) break;
        free_frame(&junk);
    }

    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(100);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    ASSERT(f.payload_len >= 76);

    /* publishes_rejected at offset 52, subscribes_rejected at offset 60 */
    uint64_t pub_rej = 0, sub_rej = 0;
    for (int b = 0; b < 8; b++)
        pub_rej = (pub_rej << 8) | f.payload[52 + b];
    for (int b = 0; b < 8; b++)
        sub_rej = (sub_rej << 8) | f.payload[60 + b];
    free_frame(&f);

    ASSERT(pub_rej >= 1);
    ASSERT(sub_rej >= 1);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, subscribe_cap_enforced) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 4;
    config.log_to_stdout = 0;
    config.max_subs_per_client = 3;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(200);

    int fd = connect_to(STATS_PORT + 4);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    ASSERT_EQ(do_subscribe(fd, parser, "cap.s1", 1), 0);
    ASSERT_EQ(do_subscribe(fd, parser, "cap.s2", 2), 0);
    ASSERT_EQ(do_subscribe(fd, parser, "cap.s3", 3), 0);

    /* 4th subscribe should be rejected: SUBACK with code=1 */
    char subj[32];
    snprintf(subj, sizeof(subj), "cap.s4");
    uint16_t slen = (uint16_t)strlen(subj);
    uint8_t sbuf[64];
    sbuf[0] = 0; sbuf[1] = 0; sbuf[2] = 0; sbuf[3] = 4; /* sub_id = 4 */
    sbuf[4] = (slen >> 8) & 0xFF;
    sbuf[5] = slen & 0xFF;
    memcpy(sbuf + 6, subj, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, sbuf, 6 + slen);
    wait_ms(100);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_SUBACK);
    ASSERT(f.payload_len >= 1);
    ASSERT_EQ(f.payload[0], 1); /* error code */
    free_frame(&f);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, ws_round_trip) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 6;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);

    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(100);

    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    cmq_parser_t *parser = cmq_parser_create();

    ws_send_cmq(fd, CMQ_OP_CONNECT, NULL, 0);
    cmq_frame_t f;
    ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_CONNACK);
    free_frame(&f);

    const char *subj = "ws.test";
    uint16_t slen = (uint16_t)strlen(subj);
    uint8_t sbuf[64];
    sbuf[0] = 0; sbuf[1] = 0; sbuf[2] = 0; sbuf[3] = 1;
    sbuf[4] = (slen >> 8) & 0xFF; sbuf[5] = slen & 0xFF;
    memcpy(sbuf + 6, subj, slen);
    ws_send_cmq(fd, CMQ_OP_SUBSCRIBE, sbuf, 6 + slen);
    ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_SUBACK);
    free_frame(&f);

    int pub = connect_to(config.port);
    ASSERT(pub >= 0);
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub, pp);

    const char *body = "hello-ws";
    uint16_t blen = (uint16_t)strlen(body);
    uint8_t pbuf[128];
    size_t off = 0;
    pbuf[off++] = (slen >> 8) & 0xFF; pbuf[off++] = slen & 0xFF;
    memcpy(pbuf + off, subj, slen); off += slen;
    pbuf[off++] = 0; pbuf[off++] = 0;
    memcpy(pbuf + off, body, blen); off += blen;
    send_frame(pub, CMQ_OP_PUBLISH, pbuf, off);
    wait_ms(150);

    ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_MESSAGE);
    free_frame(&f);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(parser);
    close(pub);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Build a masked WS binary frame carrying a CMQ op; return malloc'd buffer. */
static uint8_t *ws_build_masked(cmq_op_t op, const uint8_t *payload, size_t plen,
                                 size_t *out_len) {
    uint8_t cmq[8192];
    size_t cmq_len = cmq_frame_encode(cmq, sizeof(cmq), op, 0, payload, plen);
    if (cmq_len == 0) return NULL;
    static const uint8_t mk[4] = {0x11, 0x22, 0x33, 0x44};
    size_t hdr = (cmq_len <= 125) ? 2 : 4;
    size_t total = hdr + 4 + cmq_len;
    uint8_t *buf = malloc(total);
    if (!buf) return NULL;
    buf[0] = 0x82;
    if (cmq_len <= 125) {
        buf[1] = (uint8_t)(0x80 | cmq_len);
    } else {
        buf[1] = 0x80 | 126;
        buf[2] = (uint8_t)(cmq_len >> 8);
        buf[3] = (uint8_t)cmq_len;
    }
    buf[hdr] = mk[0]; buf[hdr + 1] = mk[1]; buf[hdr + 2] = mk[2]; buf[hdr + 3] = mk[3];
    for (size_t i = 0; i < cmq_len; i++)
        buf[hdr + 4 + i] = cmq[i] ^ mk[i % 4];
    *out_len = total;
    return buf;
}

TEST(server_ops, ws_partial_frame) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 7;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);

    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(80);
    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    /* Send CONNECT as two TCP writes to force WS reassembly. */
    size_t flen = 0;
    uint8_t *frame = ws_build_masked(CMQ_OP_CONNECT, NULL, 0, &flen);
    ASSERT(frame != NULL);
    ASSERT(flen > 4);
    size_t mid = flen / 2;
    ASSERT(write(fd, frame, mid) == (ssize_t)mid);
    wait_ms(30);
    ASSERT(write(fd, frame + mid, flen - mid) == (ssize_t)(flen - mid));
    free(frame);
    wait_ms(80);

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t f;
    ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_CONNACK);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, ws_fragmented_message) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 8;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);

    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(80);
    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    /* Split a CMQ CONNECT across BINARY(FIN=0) + CONTINUATION(FIN=1). */
    uint8_t cmq[64];
    size_t cmq_len = cmq_frame_encode(cmq, sizeof(cmq), CMQ_OP_CONNECT, 0, NULL, 0);
    ASSERT(cmq_len > 4);
    size_t mid = cmq_len / 2;
    size_t rest = cmq_len - mid;
    static const uint8_t mk[4] = {0xAA, 0xBB, 0xCC, 0xDD};

    uint8_t frag1[128];
    frag1[0] = 0x02; /* FIN=0, BINARY */
    frag1[1] = (uint8_t)(0x80 | mid);
    frag1[2] = mk[0]; frag1[3] = mk[1]; frag1[4] = mk[2]; frag1[5] = mk[3];
    for (size_t i = 0; i < mid; i++)
        frag1[6 + i] = cmq[i] ^ mk[i % 4];
    ASSERT(write(fd, frag1, 6 + mid) == (ssize_t)(6 + mid));
    wait_ms(20);

    uint8_t frag2[128];
    frag2[0] = 0x80; /* FIN=1, CONTINUATION */
    frag2[1] = (uint8_t)(0x80 | rest);
    frag2[2] = mk[0]; frag2[3] = mk[1]; frag2[4] = mk[2]; frag2[5] = mk[3];
    for (size_t i = 0; i < rest; i++)
        frag2[6 + i] = cmq[mid + i] ^ mk[i % 4];
    ASSERT(write(fd, frag2, 6 + rest) == (ssize_t)(6 + rest));
    wait_ms(80);

    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t f;
    ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_CONNACK);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* HTTP upgrade + first WS frame in one TCP write — trailing bytes must not be dropped. */
TEST(server_ops, ws_pipelined_upgrade) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 9;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);

    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    size_t flen = 0;
    uint8_t *frame = ws_build_masked(CMQ_OP_CONNECT, NULL, 0, &flen);
    ASSERT(frame != NULL);

    size_t ulen = strlen(upgrade);
    uint8_t *combo = malloc(ulen + flen);
    ASSERT(combo != NULL);
    memcpy(combo, upgrade, ulen);
    memcpy(combo + ulen, frame, flen);
    ASSERT(write(fd, combo, ulen + flen) == (ssize_t)(ulen + flen));
    free(combo);
    free(frame);
    wait_ms(120);

    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    /* CONNACK may already be in the same TCP read after 101, or arrive next. */
    cmq_parser_t *parser = cmq_parser_create();
    cmq_frame_t f;
    /* Scan remaining bytes in resp for a WS binary frame, else ws_recv_cmq. */
    int got = 0;
    for (ssize_t i = 0; i + 2 < rn; i++) {
        if ((uint8_t)resp[i] == 0x82) {
            cmq_ws_frame_t wf;
            int parsed = cmq_ws_frame_parse((const uint8_t *)resp + i,
                                             (size_t)(rn - i), &wf);
            if (parsed > 0 && wf.opcode == CMQ_WS_OPCODE_BINARY &&
                wf.payload_len > 0) {
                if (cmq_parser_feed(parser, wf.payload, wf.payload_len) == 1) {
                    const cmq_frame_t *pf = cmq_parser_frame(parser);
                    if (pf && pf->hdr.op == CMQ_OP_CONNACK) {
                        got = 1;
                        break;
                    }
                }
            }
        }
    }
    if (!got) {
        ASSERT_EQ(ws_recv_cmq(fd, parser, &f), 0);
        ASSERT_EQ(f.hdr.op, CMQ_OP_CONNACK);
        free_frame(&f);
    }

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Resubscribe same sub_id must replace, not double-deliver. */
TEST(server_ops, subscribe_replace_same_id) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 10;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int sub_fd = connect_to(config.port);
    ASSERT(sub_fd >= 0);
    cmq_parser_t *sp = cmq_parser_create();
    do_connect(sub_fd, sp);
    ASSERT_EQ(do_subscribe(sub_fd, sp, "rep.old", 1), 0);
    ASSERT_EQ(do_subscribe(sub_fd, sp, "rep.new", 1), 0); /* replace sub_id=1 */

    int pub_fd = connect_to(config.port);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    const char *old_s = "rep.old";
    const char *new_s = "rep.new";
    uint16_t olen = (uint16_t)strlen(old_s);
    uint16_t nlen = (uint16_t)strlen(new_s);
    uint8_t pbuf[128];
    size_t off = 0;
    pbuf[off++] = (olen >> 8) & 0xFF; pbuf[off++] = olen & 0xFF;
    memcpy(pbuf + off, old_s, olen); off += olen;
    pbuf[off++] = 0; pbuf[off++] = 0;
    memcpy(pbuf + off, "x", 1); off += 1;
    send_frame(pub_fd, CMQ_OP_PUBLISH, pbuf, off);
    wait_ms(80);

    /* Old subject must not deliver. */
    cmq_frame_t f;
    int got_old = (recv_frame(sub_fd, &f, sp) == 0);
    if (got_old) free_frame(&f);
    ASSERT(!got_old);

    off = 0;
    pbuf[off++] = (nlen >> 8) & 0xFF; pbuf[off++] = nlen & 0xFF;
    memcpy(pbuf + off, new_s, nlen); off += nlen;
    pbuf[off++] = 0; pbuf[off++] = 0;
    memcpy(pbuf + off, "y", 1); off += 1;
    send_frame(pub_fd, CMQ_OP_PUBLISH, pbuf, off);
    wait_ms(80);
    ASSERT_EQ(recv_frame(sub_fd, &f, sp), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_MESSAGE);
    free_frame(&f);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(sp);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Truncated batch must ERROR with no partial delivery. */
TEST(server_ops, batch_invalid_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 11;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int sub_fd = connect_to(config.port);
    ASSERT(sub_fd >= 0);
    cmq_parser_t *sp = cmq_parser_create();
    do_connect(sub_fd, sp);
    ASSERT_EQ(do_subscribe(sub_fd, sp, "batch.bad", 1), 0);

    int pub_fd = connect_to(config.port);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    /* count=2 but only one complete entry */
    const char *subj = "batch.bad";
    uint16_t slen = (uint16_t)strlen(subj);
    uint8_t batch[128];
    size_t off = 0;
    batch[off++] = 0; batch[off++] = 2;
    batch[off++] = (slen >> 8) & 0xFF; batch[off++] = slen & 0xFF;
    memcpy(batch + off, subj, slen); off += slen;
    batch[off++] = 0; batch[off++] = 0; /* reply */
    batch[off++] = 0; batch[off++] = 0; batch[off++] = 0; batch[off++] = 1;
    batch[off++] = 'z';
    send_frame(pub_fd, CMQ_OP_BATCH, batch, off);
    wait_ms(80);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(pub_fd, &f, pp), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    int got = (recv_frame(sub_fd, &f, sp) == 0);
    if (got) free_frame(&f);
    ASSERT(!got);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(sp);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, unsubscribe_unknown) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 12;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    uint8_t pl[4] = {0, 0, 0, 99};
    send_frame(fd, CMQ_OP_UNSUBSCRIBE, pl, 4);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Ops before CONNECT must be rejected (auth bypass / protocol gate). */
TEST(server_ops, require_connect_before_ops) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 13;
    config.log_to_stdout = 0;
    config.auth_username = "user";
    config.auth_password = "pass";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();

    /* Skip CONNECT; attempt SUBSCRIBE directly. */
    uint8_t sbuf[32];
    const char *subj = "noauth";
    uint16_t slen = (uint16_t)strlen(subj);
    sbuf[0] = 0; sbuf[1] = 0; sbuf[2] = 0; sbuf[3] = 1;
    sbuf[4] = (slen >> 8) & 0xFF; sbuf[5] = slen & 0xFF;
    memcpy(sbuf + 6, subj, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, sbuf, 6 + slen);
    wait_ms(100);

    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    /* May receive INFO first (NATS-like), then ERROR. */
    if (f.hdr.op == CMQ_OP_INFO) {
        free_frame(&f);
        ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    }
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Overlong queue group must SUBACK fail (not silent empty qg). */
TEST(server_ops, queue_group_too_long) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 15;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *subj = "qg.long";
    uint16_t slen = (uint16_t)strlen(subj);
    uint16_t qglen = 64; /* == CMQ_MAX_QUEUE_GROUP → reject */
    uint8_t buf[256];
    size_t off = 0;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 1; /* sub_id */
    buf[off++] = (slen >> 8) & 0xFF; buf[off++] = slen & 0xFF;
    memcpy(buf + off, subj, slen); off += slen;
    buf[off++] = (qglen >> 8) & 0xFF; buf[off++] = qglen & 0xFF;
    memset(buf + off, 'q', qglen); off += qglen;

    send_frame(fd, CMQ_OP_SUBSCRIBE, buf, off);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_SUBACK);
    ASSERT(f.payload_len >= 1);
    ASSERT_EQ(f.payload[0], 1); /* code=1 failure */
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* RFC 6455: server must answer WS PING with unmasked PONG echoing payload. */
TEST(server_ops, ws_ping_pong) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 16;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    wait_ms(20);
    const char *upgrade =
        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(100);
    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    /* Masked WS PING with 4-byte app data. */
    static const uint8_t mk[4] = {0x01, 0x02, 0x03, 0x04};
    static const uint8_t app[4] = {'p', 'i', 'n', 'g'};
    uint8_t ping[2 + 4 + 4];
    ping[0] = 0x89; /* FIN + PING */
    ping[1] = 0x80 | 4;
    ping[2] = mk[0]; ping[3] = mk[1]; ping[4] = mk[2]; ping[5] = mk[3];
    for (int i = 0; i < 4; i++) ping[6 + i] = app[i] ^ mk[i];
    ASSERT(write(fd, ping, sizeof(ping)) == (ssize_t)sizeof(ping));
    wait_ms(100);

    int got_pong = 0;
    for (int attempt = 0; attempt < 50 && !got_pong; attempt++) {
        uint8_t rbuf[64];
        ssize_t n = read(fd, rbuf, sizeof(rbuf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_ms(20);
                continue;
            }
            break;
        }
        cmq_ws_frame_t wf;
        int parsed = cmq_ws_frame_parse(rbuf, (size_t)n, &wf);
        if (parsed > 0 && wf.opcode == CMQ_WS_OPCODE_PONG &&
            wf.payload_len == 4 && !wf.masked &&
            memcmp(wf.payload, app, 4) == 0) {
            got_pong = 1;
        }
    }
    ASSERT(got_pong);

    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* TCP without CONNECT must be reaped by keepalive (slot exhaustion DoS). */
TEST(server_ops, init_idle_timeout) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 14;
    config.log_to_stdout = 0;
    config.ping_interval_ms = 200;
    config.max_clients = 2;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int idle = connect_to(config.port);
    ASSERT(idle >= 0);
    /* PING before CONNECT must not refresh INIT keepalive. */
    send_frame(idle, CMQ_OP_PING, NULL, 0);
    wait_ms(50);
    send_frame(idle, CMQ_OP_PING, NULL, 0);
    wait_ms(1200); /* > 2 * ping_interval */

    /* Slot should be free again for a real CONNECT. */
    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);
    send_frame(fd, CMQ_OP_STATS, NULL, 0);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_STATS);
    free_frame(&f);

    close(idle);
    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(server_ops, publish_invalid_subject) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 17;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *bad = "foo..bar";
    uint16_t slen = (uint16_t)strlen(bad);
    uint8_t buf[64];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF; buf[off++] = slen & 0xFF;
    memcpy(buf + off, bad, slen); off += slen;
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 'x';
    send_frame(fd, CMQ_OP_PUBLISH, buf, off);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* RFC 6455: unmasked client frames must close the connection. */
TEST(server_ops, ws_unmasked_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 18;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(100);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(80);
    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    ASSERT(rn > 0);
    resp[rn] = '\0';
    ASSERT(strstr(resp, "101") != NULL);

    uint8_t cmq[16];
    size_t cmq_len = cmq_frame_encode(cmq, sizeof(cmq), CMQ_OP_CONNECT, 0, NULL, 0);
    ASSERT(cmq_len > 0 && cmq_len <= 125);
    uint8_t frame[140];
    frame[0] = 0x82;
    frame[1] = (uint8_t)cmq_len; /* mask bit clear */
    memcpy(frame + 2, cmq, cmq_len);
    ASSERT(write(fd, frame, 2 + cmq_len) == (ssize_t)(2 + cmq_len));
    wait_ms(80);
    /* Peer should close; subsequent read returns 0 or error. */
    char junk[8];
    ssize_t n = read(fd, junk, sizeof(junk));
    ASSERT(n <= 0);

    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* CONNECT after DISCONNECT must not resurrect a CLOSING client. */
TEST(server_ops, connect_while_closing_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 19;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    send_frame(fd, CMQ_OP_DISCONNECT, NULL, 0);
    wait_ms(30);
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(80);

    /* Peer should close (or CONNACK fail then close); no successful reconnect. */
    char junk[8];
    ssize_t n = read(fd, junk, sizeof(junk));
    /* Either EOF or a failed CONNACK then teardown — connection must not stay live. */
    if (n > 0) {
        wait_ms(80);
        n = read(fd, junk, sizeof(junk));
    }
    ASSERT(n <= 0);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* tls_enabled with stub backend must fail closed. */
TEST(server_ops, tls_stub_refused) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 20;
    config.log_to_stdout = 0;
    config.tls_enabled = 1;
    config.tls_cert = "/tmp/cmq-fake-cert.pem";
    config.tls_key = "/tmp/cmq-fake-key.pem";
    cmq_server_t *srv = NULL;
    ASSERT(cmq_server_create(&srv, &config) != CMQ_OK);
    ASSERT_NULL(srv);
}

/* Publish subjects must be concrete (no * / >). */
TEST(server_ops, publish_wildcard_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 21;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *subj = "orders.*";
    uint16_t slen = (uint16_t)strlen(subj);
    uint8_t buf[64];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subj, slen); off += slen;
    buf[off++] = 0; buf[off++] = 0; /* reply */
    buf[off++] = 'x';
    send_frame(fd, CMQ_OP_PUBLISH, buf, off);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Mismatched Origin must fail the WebSocket upgrade. */
TEST(server_ops, ws_origin_mismatch_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 22;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    const char *upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Origin: http://evil.example\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT(write(fd, upgrade, strlen(upgrade)) > 0);
    wait_ms(80);
    char resp[512];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    if (rn > 0) {
        resp[rn] = '\0';
        ASSERT(strstr(resp, "101") == NULL);
    }
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* sub_id 0 must SUBACK-fail (require_sub_id treats 0 as "no check"). */
TEST(server_ops, subscribe_sub_id_zero_rejected) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 23;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *subject = "zero.sub";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t buf[64];
    memset(buf, 0, 4); /* sub_id = 0 */
    buf[4] = (slen >> 8) & 0xFF;
    buf[5] = slen & 0xFF;
    memcpy(buf + 6, subject, slen);
    send_frame(fd, CMQ_OP_SUBSCRIBE, buf, 6 + slen);
    wait_ms(50);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_SUBACK);
    ASSERT(f.payload_len >= 1);
    ASSERT(f.payload[0] != 0); /* failure code */
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* REQUEST with no subscribers must ERROR, not PUBACK. */
TEST(server_ops, request_no_responders) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 24;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *subj = "nobody.home";
    const char *reply = "_INBOX.x";
    uint16_t slen = (uint16_t)strlen(subj);
    uint16_t rlen = (uint16_t)strlen(reply);
    uint8_t buf[128];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subj, slen); off += slen;
    buf[off++] = (rlen >> 8) & 0xFF;
    buf[off++] = rlen & 0xFF;
    memcpy(buf + off, reply, rlen); off += rlen;
    buf[off++] = 'q';
    send_frame(fd, CMQ_OP_REQUEST, buf, off);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* Wildcard reply-to must be rejected (would fan-out RESPONSE). */
TEST(server_ops, request_wildcard_reply_to) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = STATS_PORT + 25;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_ms(80);

    int fd = connect_to(config.port);
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    do_connect(fd, parser);

    const char *subj = "svc.echo";
    const char *reply = "_INBOX.>";
    uint16_t slen = (uint16_t)strlen(subj);
    uint16_t rlen = (uint16_t)strlen(reply);
    uint8_t buf[128];
    size_t off = 0;
    buf[off++] = (slen >> 8) & 0xFF;
    buf[off++] = slen & 0xFF;
    memcpy(buf + off, subj, slen); off += slen;
    buf[off++] = (rlen >> 8) & 0xFF;
    buf[off++] = rlen & 0xFF;
    memcpy(buf + off, reply, rlen); off += rlen;
    buf[off++] = 'q';
    send_frame(fd, CMQ_OP_REQUEST, buf, off);
    wait_ms(80);
    cmq_frame_t f;
    ASSERT_EQ(recv_frame(fd, &f, parser), 0);
    ASSERT_EQ(f.hdr.op, CMQ_OP_ERROR);
    free_frame(&f);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
