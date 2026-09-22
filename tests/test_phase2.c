#define _POSIX_C_SOURCE 200809L
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_password.h"
#include "cmq_audit.h"
#include "cmq_test.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

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
    uint8_t buf[4096];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, 0, payload, plen);
    if (len == 0) return -1;
    return write(fd, buf, len);
}

static ssize_t send_frame_flags(int fd, cmq_op_t op, uint8_t flags,
                                  const uint8_t *payload, size_t plen) {
    uint8_t buf[8192];
    size_t len = cmq_frame_encode(buf, sizeof(buf), op, flags, payload, plen);
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

static void *server_thread(void *arg) {
    cmq_server_t *srv = arg;
    cmq_server_run(srv);
    return NULL;
}

static void wait_server(void) {
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
}

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

/* Encode a SUBSCRIBE frame into `out`. Returns the total length. */
static size_t build_sub_frame(uint32_t sub_id, const char *subj,
                                uint8_t *out) {
    size_t off = 0;
    out[off++] = (sub_id >> 24) & 0xFF;
    out[off++] = (sub_id >> 16) & 0xFF;
    out[off++] = (sub_id >> 8) & 0xFF;
    out[off++] = sub_id & 0xFF;
    uint16_t slen = (uint16_t)strlen(subj);
    out[off++] = (slen >> 8) & 0xFF;
    out[off++] = slen & 0xFF;
    memcpy(out + off, subj, slen);
    off += slen;
    return off;
}

static void do_connect(int fd, cmq_parser_t *parser) {
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);
}

TEST(phase2, auth_success) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18901;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18901);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "secret";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);

    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, auth_failure) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18902;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18902);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "wrong";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);

    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 2);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, hashed_auth_success) {
    char stored[256];
    ASSERT_EQ(cmq_password_hash("secret", stored, sizeof(stored)), 0);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18904;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = stored;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18904);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "secret";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);
    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    }
    ASSERT_EQ(frame.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(frame.payload[0], 0);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* RED v0.6.10: cmq_quota_check_connect must reject CONNECTs once
 * max_connections_per_account is exceeded. The quota object is built
 * by cmq_server_create and the check function exists in cmq_quota.h,
 * but cmq_server.c never calls it on the CONNECT path. The second
 * CONNECT in the same account must receive a non-zero CONNACK. */
TEST(phase2, quota_rejects_connect_above_per_account_cap) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18908;
    config.log_to_stdout = 0;
    config.max_connections_per_account = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    /* First CONNECT should succeed; second should be rejected. */
    int fd1 = connect_to(18908);
    ASSERT(fd1 >= 0);
    wait_server();
    cmq_parser_t *p1 = cmq_parser_create();
    send_frame(fd1, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t f1;
    ASSERT_EQ(recv_frame(fd1, &f1, p1), 0);
    if (f1.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&f1);
        ASSERT_EQ(recv_frame(fd1, &f1, p1), 0);
    }
    ASSERT_EQ(f1.hdr.op, CMQ_OP_CONNACK);
    ASSERT_EQ(f1.payload[0], 0);
    free_frame_payload(&f1);
    cmq_parser_destroy(p1);

    int fd2 = connect_to(18908);
    ASSERT(fd2 >= 0);
    wait_server();
    cmq_parser_t *p2 = cmq_parser_create();
    send_frame(fd2, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t f2;
    ASSERT_EQ(recv_frame(fd2, &f2, p2), 0);
    if (f2.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&f2);
        ASSERT_EQ(recv_frame(fd2, &f2, p2), 0);
    }
    ASSERT_EQ(f2.hdr.op, CMQ_OP_CONNACK);
    ASSERT(f2.payload[0] != 0);
    free_frame_payload(&f2);
    cmq_parser_destroy(p2);

    close(fd1);
    close(fd2);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* RED: a successful CONNECT must emit CMQ_AUDIT_AUTH_OK and a failed
 * CONNECT must emit CMQ_AUDIT_AUTH_FAIL into the configured audit
 * file. Currently neither event is logged because the server never
 * calls cmq_audit_log for these enum values. */
static int audit_file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* Locate "trace":"<hex>" inside an event line. Returns the hex length
 * (32 when propagation is correct) or -1 when the field is missing
 * / not a 32-char lowercase-hex string. */
static int audit_event_trace_hex_len(const char *path, const char *event_name) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    char needle[64];
    snprintf(needle, sizeof(needle), "\"event\":\"%s\"", event_name);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    const char *trace = strstr(p, "\"trace\":\"");
    if (!trace) return -1;
    trace += strlen("\"trace\":\"");
    int len = 0;
    while (len < 33 &&
           ((trace[len] >= '0' && trace[len] <= '9') ||
            (trace[len] >= 'a' && trace[len] <= 'f'))) {
        len++;
    }
    if (len == 32 && trace[len] == '"') return 32;
    return len > 0 ? -len : -1;
}

TEST(phase2, audit_emits_auth_ok_on_success) {
    const char *audit_path = "/tmp/cmq-test-phase2-audit-ok.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18906;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18906);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "secret";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);
    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(100);

    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    ASSERT(audit_file_contains(audit_path, "\"event\":\"auth_ok\""));

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

TEST(phase2, audit_emits_auth_fail_on_bad_password) {
    const char *audit_path = "/tmp/cmq-test-phase2-audit-fail.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18907;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18907);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "wrong";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);
    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(100);

    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    ASSERT(audit_file_contains(audit_path, "\"event\":\"auth_fail\""));
    /* The audit line must also carry the F11 connection trace id. */
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "auth_fail"), 32);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

TEST(phase2, queue_group_delivery) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18903;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int sub1 = connect_to(18903);
    int sub2 = connect_to(18903);
    ASSERT(sub1 >= 0);
    ASSERT(sub2 >= 0);
    wait_server();

    cmq_parser_t *p1 = cmq_parser_create();
    cmq_parser_t *p2 = cmq_parser_create();
    do_connect(sub1, p1);
    do_connect(sub2, p2);

    const char *subject = "work.tasks";
    uint16_t slen = (uint16_t)strlen(subject);
    const char *queue = "workers";
    uint16_t qlen = (uint16_t)strlen(queue);

    uint8_t sub_pl[128];
    sub_pl[0] = 0; sub_pl[1] = 0; sub_pl[2] = 0; sub_pl[3] = 1;
    sub_pl[4] = (slen >> 8) & 0xFF; sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    sub_pl[6 + slen] = (qlen >> 8) & 0xFF;
    sub_pl[6 + slen + 1] = qlen & 0xFF;
    memcpy(sub_pl + 6 + slen + 2, queue, qlen);
    send_frame(sub1, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen + 2 + qlen);
    wait_ms(50);

    cmq_frame_t frame;
    recv_frame(sub1, &frame, p1);
    free_frame_payload(&frame);

    sub_pl[3] = 2;
    send_frame(sub2, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen + 2 + qlen);
    wait_ms(50);
    recv_frame(sub2, &frame, p2);
    free_frame_payload(&frame);

    int pub_fd = connect_to(18903);
    ASSERT(pub_fd >= 0);
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    uint8_t pub_pl[64];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0;
    pub_pl[off++] = 0;
    const char *msg = "task1";
    memcpy(pub_pl + off, msg, strlen(msg));
    off += strlen(msg);

    send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
    wait_ms(100);

    int got_sub1 = (recv_frame(sub1, &frame, p1) == 0);
    if (got_sub1) free_frame_payload(&frame);
    int got_sub2 = (recv_frame(sub2, &frame, p2) == 0);
    if (got_sub2) free_frame_payload(&frame);

    ASSERT(got_sub1 + got_sub2 == 1);

    /* Round-robin: subsequent publishes should reach both members. */
    int n1 = got_sub1, n2 = got_sub2;
    for (int i = 0; i < 19; i++) {
        send_frame(pub_fd, CMQ_OP_PUBLISH, pub_pl, off);
        wait_ms(40);
        if (recv_frame(sub1, &frame, p1) == 0) {
            n1++;
            free_frame_payload(&frame);
        }
        if (recv_frame(sub2, &frame, p2) == 0) {
            n2++;
            free_frame_payload(&frame);
        }
    }
    ASSERT_EQ(n1 + n2, 20);
    ASSERT(n1 >= 1 && n2 >= 1);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(p1);
    cmq_parser_destroy(p2);
    close(pub_fd);
    close(sub1);
    close(sub2);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, headers_passthrough) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18904;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int sub_fd = connect_to(18904);
    ASSERT(sub_fd >= 0);
    wait_server();
    cmq_parser_t *sp = cmq_parser_create();
    do_connect(sub_fd, sp);

    const char *subject = "hdr.test";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t sub_pl[64];
    sub_pl[0] = 0; sub_pl[1] = 0; sub_pl[2] = 0; sub_pl[3] = 1;
    sub_pl[4] = (slen >> 8) & 0xFF; sub_pl[5] = slen & 0xFF;
    memcpy(sub_pl + 6, subject, slen);
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + slen);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(sub_fd, &frame, sp);
    free_frame_payload(&frame);

    int pub_fd = connect_to(18904);
    ASSERT(pub_fd >= 0);
    wait_server();
    cmq_parser_t *pp = cmq_parser_create();
    do_connect(pub_fd, pp);

    const char *hdr_key = "X-Id";
    const char *hdr_val = "42";
    size_t hdr_total = 1 + strlen(hdr_key) + 1 + strlen(hdr_val);
    const char *msg = "hello";
    size_t msg_len = strlen(msg);

    uint8_t pub_pl[256];
    size_t poff = 0;
    pub_pl[poff++] = (slen >> 8) & 0xFF;
    pub_pl[poff++] = slen & 0xFF;
    memcpy(pub_pl + poff, subject, slen);
    poff += slen;
    pub_pl[poff++] = 0;
    pub_pl[poff++] = 0;
    pub_pl[poff++] = (hdr_total >> 8) & 0xFF;
    pub_pl[poff++] = hdr_total & 0xFF;
    pub_pl[poff++] = (uint8_t)strlen(hdr_key);
    memcpy(pub_pl + poff, hdr_key, strlen(hdr_key));
    poff += strlen(hdr_key);
    pub_pl[poff++] = (uint8_t)strlen(hdr_val);
    memcpy(pub_pl + poff, hdr_val, strlen(hdr_val));
    poff += strlen(hdr_val);
    memcpy(pub_pl + poff, msg, msg_len);
    poff += msg_len;

    send_frame_flags(pub_fd, CMQ_OP_PUBLISH, CMQ_FLAG_HEADERS, pub_pl, poff);
    wait_ms(100);

    ASSERT_EQ(recv_frame(sub_fd, &frame, sp), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_MESSAGE);
    ASSERT(frame.hdr.flags & CMQ_FLAG_HEADERS);
    free_frame_payload(&frame);

    cmq_parser_destroy(pp);
    cmq_parser_destroy(sp);
    close(pub_fd);
    close(sub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST(phase2, info_has_stats) {
    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18905;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18905);
    ASSERT(fd >= 0);
    wait_server();

    cmq_parser_t *parser = cmq_parser_create();
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_server();
    cmq_frame_t frame;
    ASSERT_EQ(recv_frame(fd, &frame, parser), 0);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_INFO);
    ASSERT(frame.payload_len > 0);
    ASSERT(memchr(frame.payload, '{', frame.payload_len) != NULL);

    char *json = malloc(frame.payload_len + 1);
    memcpy(json, frame.payload, frame.payload_len);
    json[frame.payload_len] = '\0';
    ASSERT(strstr(json, "\"version\"") != NULL);
    ASSERT(strstr(json, "\"connections\"") != NULL);
    ASSERT(strstr(json, "\"subscriptions\"") != NULL);
    free(json);
    free_frame_payload(&frame);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

/* RED v0.6.16: when a PUBLISH is rejected by the per-account
 * publish quota, the server must emit CMQ_AUDIT_RATE_LIMIT_REJECT
 * with the offending subject so operators can see quota pressure in
 * the audit log. Today handle_publish only updates the
 * stat_publishes_rejected counter and emits no audit event. */
TEST(phase2, audit_emits_rate_limit_on_publish_quota) {
    const char *audit_path = "/tmp/cmq-test-v0616-audit.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18910;
    config.log_to_stdout = 0;
    /* Cap at 1 message/sec so the second publish is rejected. */
    config.max_msgs_per_sec_per_account = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18910);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    /* First CONNECT + PUBLISH succeeds. */
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    const char *subject = "quota.audit";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t pub_pl[128];
    pub_pl[0] = (slen >> 8) & 0xFF;
    pub_pl[1] = slen & 0xFF;
    memcpy(pub_pl + 2, subject, slen);
    pub_pl[2 + slen] = 0; pub_pl[3 + slen] = 0;
    const char *body1 = "first";
    uint16_t blen1 = (uint16_t)strlen(body1);
    pub_pl[4 + slen] = (blen1 >> 24) & 0xFF;
    pub_pl[5 + slen] = (blen1 >> 16) & 0xFF;
    pub_pl[6 + slen] = (blen1 >> 8) & 0xFF;
    pub_pl[7 + slen] = blen1 & 0xFF;
    memcpy(pub_pl + 8 + slen, body1, blen1);
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, 8 + slen + blen1);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    /* Second PUBLISH exceeds the per-account 1 msg/sec cap → ERROR +
     * rate_limit_reject audit. */
    const char *body2 = "second";
    uint16_t blen2 = (uint16_t)strlen(body2);
    pub_pl[4 + slen] = (blen2 >> 24) & 0xFF;
    pub_pl[5 + slen] = (blen2 >> 16) & 0xFF;
    pub_pl[6 + slen] = (blen2 >> 8) & 0xFF;
    pub_pl[7 + slen] = blen2 & 0xFF;
    memcpy(pub_pl + 8 + slen, body2, blen2);
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, 8 + slen + blen2);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_ERROR);
    free_frame_payload(&frame);

    /* Poll the audit file for the rate_limit_reject event. */
    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "\"subject\":\"publish\"") != NULL);
    ASSERT(strstr(buf, "quota exceeded") != NULL);
}

/* RED v0.6.17: handle_frame's CONNECT branch assigns a 16-byte trace
 * id at cmq_server.c:4626-4630, but the loop logic is inverted:
 * "assigned = 1" runs when ANY byte is zero, which is true for
 * zero-initialized new clients. cmq_trace_id is therefore never
 * called and every audit event for a new client passes a NULL
 * trace_id. After the fix, the audit_ok event for a successful
 * CONNECT must carry a 32-char lowercase-hex trace string. */
TEST(phase2, connect_assigns_trace_id_in_audit) {
    const char *audit_path = "/tmp/cmq-test-v0617-trace-audit.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18911;
    config.log_to_stdout = 0;
    config.auth_username = "admin";
    config.auth_password = "secret";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18911);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    const char *user = "admin";
    const char *pass = "secret";
    uint8_t connect_pl[256];
    uint16_t ulen = (uint16_t)strlen(user);
    uint16_t plen = (uint16_t)strlen(pass);
    connect_pl[0] = (ulen >> 8) & 0xFF;
    connect_pl[1] = ulen & 0xFF;
    connect_pl[2] = (plen >> 8) & 0xFF;
    connect_pl[3] = plen & 0xFF;
    memcpy(connect_pl + 4, user, ulen);
    memcpy(connect_pl + 4 + ulen, pass, plen);
    send_frame(fd, CMQ_OP_CONNECT, connect_pl, 4 + ulen + plen);
    wait_ms(50);

    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    /* Poll the audit file for the auth_ok event and assert that the
     * "trace" field is a non-empty 32-char lowercase-hex string. */
    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    const char *event = strstr(buf, "\"event\":\"auth_ok\"");
    ASSERT_NOT_NULL(event);
    const char *trace_field = strstr(event, "\"trace\":\"");
    ASSERT_NOT_NULL(trace_field);
    trace_field += strlen("\"trace\":\"");
    /* Trace id hex should be 32 lowercase hex chars followed by '"'. */
    int hex_len = 0;
    while (hex_len < 33 &&
           ((trace_field[hex_len] >= '0' && trace_field[hex_len] <= '9') ||
            (trace_field[hex_len] >= 'a' && trace_field[hex_len] <= 'f'))) {
        hex_len++;
    }
    ASSERT_EQ(hex_len, 32);
    ASSERT_EQ(trace_field[hex_len], '"');

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.19: when a PUBLISH is rejected by the per-subject
 * rate limit (N1), the server must emit CMQ_AUDIT_RATE_LIMIT_REJECT
 * with the offending subject so operators can spot noisy subjects
 * in the audit log. Today handle_publish only updates the
 * stat_publishes_rejected[_ratelimit] counters and emits no audit
 * event. */
TEST(phase2, audit_emits_rate_limit_on_subject_ratelimit) {
    const char *audit_path = "/tmp/cmq-test-v0619-audit-rl.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18912;
    config.log_to_stdout = 0;
    /* Cap at 1 message/sec per subject so the second publish is
     * rejected. Per-subject rate limit is independent of the global
     * per-account quota (v0.6.16), so we set the latter to 0 to
     * isolate this test. */
    config.max_msgs_per_sec_per_subject = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18912);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    /* CONNECT + first publish accepted (within the per-subject cap). */
    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    const char *subject = "noisy.subject";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t pub_pl[128];
    pub_pl[0] = (slen >> 8) & 0xFF;
    pub_pl[1] = slen & 0xFF;
    memcpy(pub_pl + 2, subject, slen);
    pub_pl[2 + slen] = 0; pub_pl[3 + slen] = 0;
    const char *body1 = "first";
    uint16_t blen1 = (uint16_t)strlen(body1);
    pub_pl[4 + slen] = (blen1 >> 24) & 0xFF;
    pub_pl[5 + slen] = (blen1 >> 16) & 0xFF;
    pub_pl[6 + slen] = (blen1 >> 8) & 0xFF;
    pub_pl[7 + slen] = blen1 & 0xFF;
    memcpy(pub_pl + 8 + slen, body1, blen1);
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, 8 + slen + blen1);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    /* Second publish to the same subject exceeds the cap → ERROR +
     * rate_limit_reject audit. */
    const char *body2 = "second";
    uint16_t blen2 = (uint16_t)strlen(body2);
    pub_pl[4 + slen] = (blen2 >> 24) & 0xFF;
    pub_pl[5 + slen] = (blen2 >> 16) & 0xFF;
    pub_pl[6 + slen] = (blen2 >> 8) & 0xFF;
    pub_pl[7 + slen] = blen2 & 0xFF;
    memcpy(pub_pl + 8 + slen, body2, blen2);
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, 8 + slen + blen2);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_ERROR);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "noisy.subject") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);


    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.20: when the F15 per-conn inbox budget rejects a REQUEST
 * (pending >= inbox_max_pending), the server must emit a
 * rate_limit_reject audit so operators can spot slow responders
 * holding the head-of-line lock. Today handle_request only updates
 * stat_publishes_rejected and emits no audit event. */
TEST(phase2, audit_emits_rate_limit_on_inbox_full) {
    const char *audit_path = "/tmp/cmq-test-v0620-audit-inbox.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18913;
    config.log_to_stdout = 0;
    /* Cap pending REQUESTs at 1. A subscriber (second connection)
     * holds the inbox slot open until RESPONSE arrives, so two
     * back-to-back REQUESTs from the first connection trip the cap. */
    config.inbox_max_pending = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    /* Subscriber connection. */
    int sub_fd = connect_to(18913);
    ASSERT(sub_fd >= 0);
    wait_server();
    cmq_parser_t *sub_parser = cmq_parser_create();
    send_frame(sub_fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(sub_fd, &frame, sub_parser);
    free_frame_payload(&frame);
    /* Subscribe to the request subject so it consumes the inbox. */
    uint16_t sslen = 9; /* "slow.rsdr" */
    uint8_t sub_pl[64];
    sub_pl[0] = 0; sub_pl[1] = 0; sub_pl[2] = 0; sub_pl[3] = 1;
    sub_pl[4] = (sslen >> 8) & 0xFF; sub_pl[5] = sslen & 0xFF;
    memcpy(sub_pl + 6, "slow.rsdr", sslen);
    send_frame(sub_fd, CMQ_OP_SUBSCRIBE, sub_pl, 6 + sslen);
    wait_ms(50);
    recv_frame(sub_fd, &frame, sub_parser);
    free_frame_payload(&frame);

    /* Publisher connection. */
    int pub_fd = connect_to(18913);
    ASSERT(pub_fd >= 0);
    wait_server();
    cmq_parser_t *pub_parser = cmq_parser_create();
    send_frame(pub_fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    recv_frame(pub_fd, &frame, pub_parser);
    free_frame_payload(&frame);

    /* Two REQUESTs — second should be rejected as inbox full. */
    uint16_t slen = 9; /* "slow.rsdr" */
    uint16_t rlen = 5; /* empty reply "_INBOX" */
    uint8_t req_pl[64];
    size_t off = 0;
    req_pl[off++] = (slen >> 8) & 0xFF;
    req_pl[off++] = slen & 0xFF;
    memcpy(req_pl + off, "slow.rsdr", slen);
    off += slen;
    req_pl[off++] = (rlen >> 8) & 0xFF;
    req_pl[off++] = rlen & 0xFF;
    memcpy(req_pl + off, "reply", rlen);
    off += rlen;
    send_frame(pub_fd, CMQ_OP_REQUEST, req_pl, off);
    wait_ms(50);
    recv_frame(pub_fd, &frame, pub_parser);
    free_frame_payload(&frame);

    send_frame(pub_fd, CMQ_OP_REQUEST, req_pl, off);
    wait_ms(50);
    recv_frame(pub_fd, &frame, pub_parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_ERROR);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "inbox full") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);

    cmq_parser_destroy(pub_parser);
    cmq_parser_destroy(sub_parser);
    close(pub_fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    close(sub_fd);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.21: when a PUBLISH exceeds max_payload_size, the server
 * must emit a rate_limit_reject audit so operators can spot payloads
 * that the publisher never intended to ship that big. Today
 * handle_publish only updates stat_publishes_rejected_size and emits
 * no audit event. */
TEST(phase2, audit_emits_rate_limit_on_publish_too_large) {
    const char *audit_path = "/tmp/cmq-test-v0621-audit-large.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18914;
    config.log_to_stdout = 0;
    /* Frame overhead: subject (2B len + 2B reply_len) + body. */
    config.max_payload_size = 16;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18914);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    /* Body 64 B > max_payload_size 16 → rejected. */
    const char *subject = "big.payload";
    uint16_t slen = (uint16_t)strlen(subject);
    char body[64];
    memset(body, 'x', sizeof(body));
    uint32_t blen = (uint32_t)sizeof(body);
    uint8_t pub_pl[128];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0; pub_pl[off++] = 0;
    pub_pl[off++] = (blen >> 24) & 0xFF;
    pub_pl[off++] = (blen >> 16) & 0xFF;
    pub_pl[off++] = (blen >> 8) & 0xFF;
    pub_pl[off++] = blen & 0xFF;
    memcpy(pub_pl + off, body, blen);
    off += blen;
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, off);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_ERROR);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "payload too large") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.22: when handle_publish rejects with
 * cmq_account_can_export, the server must emit an audit event so
 * operators can spot accounts blocked by the export allow-list.
 * Today only stat_publishes_rejected / stat_publishes_rejected_acl
 * are updated. */
TEST(phase2, audit_emits_rate_limit_on_account_export_acl) {
    const char *audit_path = "/tmp/cmq-test-v0622-audit-acl.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18915;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    /* Restrict the default account ("$default") to a subject that the
     * test publish will not match, triggering can_export denial. The
     * destination must be a non-empty peer ("*" is the catch-all). */
    ASSERT_EQ(cmq_account_add_export(srv->accounts, "$default",
                                     "allowed.subject", "*"), 0);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18915);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    /* Publish to "denied.subject" — the allow-list restricts to
     * "allowed.subject", so can_export returns 0 and the server
     * emits "permission denied". */
    const char *subject = "denied.subject";
    uint16_t slen = (uint16_t)strlen(subject);
    const char *body = "x";
    uint16_t blen = (uint16_t)strlen(body);
    uint8_t pub_pl[128];
    size_t off = 0;
    pub_pl[off++] = (slen >> 8) & 0xFF;
    pub_pl[off++] = slen & 0xFF;
    memcpy(pub_pl + off, subject, slen);
    off += slen;
    pub_pl[off++] = 0; pub_pl[off++] = 0;
    pub_pl[off++] = (blen >> 24) & 0xFF;
    pub_pl[off++] = (blen >> 16) & 0xFF;
    pub_pl[off++] = (blen >> 8) & 0xFF;
    pub_pl[off++] = blen & 0xFF;
    memcpy(pub_pl + off, body, blen);
    off += blen;
    send_frame(fd, CMQ_OP_PUBLISH, pub_pl, off);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_ERROR);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "denied.subject") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.23: when handle_subscribe rejects a connection that has
 * already hit max_subs_per_client, the server must emit an audit
 * event so operators can spot clients exhausting their subscription
 * budget. Today only stat_subscribes_rejected is updated. */
TEST(phase2, audit_emits_rate_limit_on_subscribe_cap) {
    const char *audit_path = "/tmp/cmq-test-v0623-audit-subcap.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18916;
    config.log_to_stdout = 0;
    config.max_subs_per_client = 1;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18916);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    /* Build SUBSCRIBE frame: 4-byte sub_id, 2-byte subject len,
     * subject, no reply_to (0). */
    uint8_t sub_pl1[128];
    size_t sub_off1 = build_sub_frame(1, "alpha", sub_pl1);
    send_frame(fd, CMQ_OP_SUBSCRIBE, sub_pl1, sub_off1);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    free_frame_payload(&frame);

    /* Second distinct SUBSCRIBE exceeds the cap. */
    uint8_t sub_pl2[128];
    size_t sub_off2 = build_sub_frame(2, "beta", sub_pl2);
    send_frame(fd, CMQ_OP_SUBSCRIBE, sub_pl2, sub_off2);
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_SUBACK);
    /* SUBACK code 1 = rejected. Payload[0] is the response code. */
    ASSERT(frame.payload_len >= 5);
    ASSERT_EQ(frame.payload[0], 1);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "beta") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

/* RED v0.6.24: when handle_subscribe rejects (malformed frame,
 * empty subject, or any other validation branch) the server must
 * emit an audit event so operators can spot clients sending bad
 * SUBSCRIBE frames. Today the rejection branches only update
 * stat_subscribes_rejected and emit no audit event. */
TEST(phase2, audit_emits_rate_limit_on_malformed_subscribe) {
    const char *audit_path = "/tmp/cmq-test-v0624-audit-subacl.log";
    unlink(audit_path);
    cmq_audit_set_path(audit_path);

    cmq_config_t config = {0};
    config.num_threads = 1;
    config.host = "127.0.0.1";
    config.port = 18917;
    config.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &config), CMQ_OK);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    wait_server();

    int fd = connect_to(18917);
    ASSERT(fd >= 0);
    wait_server();
    cmq_parser_t *parser = cmq_parser_create();

    send_frame(fd, CMQ_OP_CONNECT, NULL, 0);
    wait_ms(50);
    cmq_frame_t frame;
    recv_frame(fd, &frame, parser);
    if (frame.hdr.op == CMQ_OP_INFO) {
        free_frame_payload(&frame);
        recv_frame(fd, &frame, parser);
    }
    free_frame_payload(&frame);

    /* Malformed SUBSCRIBE: payload length < 6 bytes triggers the
     * early reject at handle_subscribe's "frame->payload_len < 6"
     * guard. */
    uint8_t bad_sub[3] = {0, 0, 0};
    send_frame(fd, CMQ_OP_SUBSCRIBE, bad_sub, sizeof(bad_sub));
    wait_ms(50);
    recv_frame(fd, &frame, parser);
    ASSERT_EQ(frame.hdr.op, CMQ_OP_SUBACK);
    ASSERT(frame.payload_len >= 5);
    ASSERT_EQ(frame.payload[0], 1);
    free_frame_payload(&frame);

    struct stat st;
    int rc = -1;
    for (int i = 0; i < 50; i++) {
        rc = stat(audit_path, &st);
        if (rc == 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(rc, 0);
    FILE *audit = fopen(audit_path, "r");
    ASSERT_NOT_NULL(audit);
    char buf[8192];
    size_t alen = fread(buf, 1, sizeof(buf) - 1, audit);
    buf[alen] = '\0';
    fclose(audit);
    ASSERT(strstr(buf, "\"event\":\"rate_limit_reject\"") != NULL);
    ASSERT(strstr(buf, "malformed subscribe") != NULL);
    ASSERT_EQ(audit_event_trace_hex_len(audit_path, "rate_limit_reject"), 32);

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    cmq_audit_set_path(NULL);
    unlink(audit_path);
}

TEST_MAIN()
