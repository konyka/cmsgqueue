/* P3: F18 persistent subscription recovery on restart.
 *
 * Phase 1: server starts with persist_dir, no subs file.
 * Phase 2: pre-write a SUB record via cmq_sublist_persist_record_sub.
 * Phase 3: destroy server.
 * Phase 4: new server with same persist_dir — load path should re-insert
 *   the sub into srv->sublist (test via cmq_sublist_match).
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_sublist.h"
#include "cmq_sublist_persist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define P3_DIR "/tmp/cmq-test-p3-sublist-recover"
#define P3_WIRE_PORT 19989

static void *server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

static void wait_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_frame(int fd, cmq_op_t op, const uint8_t *payload,
                      size_t payload_len) {
    uint8_t wire[512];
    size_t wire_len = cmq_frame_encode(wire, sizeof(wire), op, 0,
                                       payload, payload_len);
    if (wire_len == 0) return -1;
    size_t sent = 0;
    while (sent < wire_len) {
        ssize_t n = write(fd, wire + sent, wire_len - sent);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_ms(5);
        } else {
            return -1;
        }
    }
    return 0;
}

static int recv_frame(int fd, cmq_parser_t *parser, cmq_op_t expected_op,
                      uint8_t *payload, size_t payload_size,
                      size_t *payload_len) {
    for (int retry = 0; retry < 200; retry++) {
        const cmq_frame_t *frame = cmq_parser_frame(parser);
        if (frame) {
            if (frame->hdr.op != expected_op || frame->payload_len > payload_size)
                return -1;
            if (frame->payload_len > 0)
                memcpy(payload, frame->payload, frame->payload_len);
            *payload_len = frame->payload_len;
            cmq_parser_next(parser);
            return 0;
        }
        uint8_t buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            (void)cmq_parser_feed(parser, buf, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_ms(5);
        } else {
            return -1;
        }
    }
    return -1;
}

static int recv_frame_op(int fd, cmq_parser_t *parser, cmq_op_t *op,
                         uint8_t *payload, size_t payload_size,
                         size_t *payload_len) {
    for (int retry = 0; retry < 200; retry++) {
        const cmq_frame_t *frame = cmq_parser_frame(parser);
        if (frame) {
            if (frame->payload_len > payload_size) return -1;
            *op = (cmq_op_t)frame->hdr.op;
            if (frame->payload_len > 0)
                memcpy(payload, frame->payload, frame->payload_len);
            *payload_len = frame->payload_len;
            cmq_parser_next(parser);
            return 0;
        }
        uint8_t buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            (void)cmq_parser_feed(parser, buf, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_ms(5);
        } else {
            return -1;
        }
    }
    return -1;
}

static int recv_connect_ack(int fd, cmq_parser_t *parser) {
    uint8_t payload[512];
    size_t payload_len = 0;
    cmq_op_t op = 0;
    if (recv_frame_op(fd, parser, &op, payload, sizeof(payload),
                      &payload_len) != 0)
        return -1;
    if (op == CMQ_OP_CONNACK) return 0;
    if (op != CMQ_OP_INFO) return -1;
    return recv_frame(fd, parser, CMQ_OP_CONNACK, payload, sizeof(payload),
                      &payload_len);
}

static int read_wal(char *buf, size_t size) {
    char path[256];
    snprintf(path, sizeof(path), "%s/cmq-subs.wal", P3_DIR);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t n = fread(buf, 1, size - 1, fp);
    buf[n] = '\0';
    int error = ferror(fp);
    fclose(fp);
    return error ? -1 : 0;
}

static int write_wal_line(const char *line) {
    char path[256];
    snprintf(path, sizeof(path), "%s/cmq-subs.wal", P3_DIR);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int rc = fputs(line, fp) == EOF ? -1 : 0;
    fclose(fp);
    return rc;
}

TEST(sublist_persist_wire, restart_restores_sub) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);

    /* Phase 1+2: write a SUB record directly. */
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(P3_DIR);
    ASSERT_NOT_NULL(p);
    uint64_t sub_id = 42;
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, sub_id, "recover.foo",
                                               "$default"), 0);
    cmq_sublist_persist_close(p);

    /* Phase 3+4: build a server with the same persist_dir. The load
     * path in cmq_server_create must re-insert "recover.foo" into
     * srv->sublist via cmq_sublist_recover_cb. */
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19994;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Verify the subject is in srv->sublist (recover path ran). */
    cmq_sublist_result_t result = {0};
    int rc = cmq_sublist_match(srv->sublist, "recover.foo", &result);
    ASSERT_EQ(rc, 0);
    /* The match itself returns the count; we want >= 1 (ghost ref). */
    printf("  match count = %zu\n", result.count);
    ASSERT(result.count >= 1);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, unsubscribe_ghost_refs_persist) {
    int rc __attribute__((unused)) = system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);

    cmq_sublist_persist_t *p = cmq_sublist_persist_open(P3_DIR);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 7, "remove.foo",
                                               "$default"), 0);
    ASSERT_EQ(cmq_sublist_persist_record_unsub(p, 7), 0);
    cmq_sublist_persist_close(p);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19993;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    cmq_sublist_result_t result = {0};
    cmq_sublist_match(srv->sublist, "remove.foo", &result);
    printf("  match count after unsub replay = %zu\n",
           result.count);
    ASSERT_EQ(result.count, 0);
    ASSERT_EQ(cmq_sublist_count(srv->sublist), 0);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " P3_DIR);
    (void)rc2;
}

TEST(sublist_persist_wire, live_wire_handlers_append_wal_and_recover_final_state) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = P3_WIRE_PORT;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    int fd = -1;
    for (int i = 0; i < 400 && fd < 0; i++) {
        fd = connect_tcp(P3_WIRE_PORT);
        if (fd < 0) wait_ms(5);
    }
    ASSERT(fd >= 0);
    cmq_parser_t *parser = cmq_parser_create();
    ASSERT_NOT_NULL(parser);

    wait_ms(100);
    ASSERT_EQ(send_frame(fd, CMQ_OP_CONNECT, NULL, 0), 0);
    uint8_t response[64];
    size_t response_len = 0;
    ASSERT_EQ(recv_connect_ack(fd, parser), 0);

    const char *subject = "live.recover";
    uint32_t sub_id = 73;
    uint16_t subject_len = (uint16_t)strlen(subject);
    uint8_t sub_payload[256];
    sub_payload[0] = (uint8_t)(sub_id >> 24);
    sub_payload[1] = (uint8_t)(sub_id >> 16);
    sub_payload[2] = (uint8_t)(sub_id >> 8);
    sub_payload[3] = (uint8_t)sub_id;
    sub_payload[4] = (uint8_t)(subject_len >> 8);
    sub_payload[5] = (uint8_t)subject_len;
    memcpy(sub_payload + 6, subject, subject_len);
    ASSERT_EQ(send_frame(fd, CMQ_OP_SUBSCRIBE, sub_payload, 6 + subject_len), 0);
    ASSERT_EQ(recv_frame(fd, parser, CMQ_OP_SUBACK, response, sizeof(response),
                         &response_len), 0);
    ASSERT_EQ(response_len, 5);
    ASSERT_EQ(response[0], 0);
    ASSERT_EQ(response[1], sub_payload[0]);
    ASSERT_EQ(response[2], sub_payload[1]);
    ASSERT_EQ(response[3], sub_payload[2]);
    ASSERT_EQ(response[4], sub_payload[3]);

    char wal[512];
    ASSERT_EQ(read_wal(wal, sizeof(wal)), 0);
    ASSERT_STR_EQ(wal, "S 73 live.recover $default\n");

    uint8_t unsub_payload[4] = {sub_payload[0], sub_payload[1],
                                sub_payload[2], sub_payload[3]};
    ASSERT_EQ(send_frame(fd, CMQ_OP_UNSUBSCRIBE, unsub_payload,
                         sizeof(unsub_payload)), 0);
    ASSERT_EQ(recv_frame(fd, parser, CMQ_OP_UNSUBACK, response, sizeof(response),
                         &response_len), 0);
    ASSERT_EQ(response_len, sizeof(unsub_payload));
    ASSERT_EQ(memcmp(response, unsub_payload, sizeof(unsub_payload)), 0);

    ASSERT_EQ(read_wal(wal, sizeof(wal)), 0);
    ASSERT_STR_EQ(wal, "S 73 live.recover $default\nU 73\n");

    cmq_parser_destroy(parser);
    close(fd);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);

    srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    cmq_sublist_result_t result = {0};
    ASSERT_EQ(cmq_sublist_match(srv->sublist, subject, &result), 0);
    ASSERT_EQ(result.count, 0);
    cmq_sublist_result_free(&result);
    cmq_server_destroy(srv);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_overlong_subject_without_ref) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    char subject[320];
    memset(subject, 'a', sizeof(subject) - 1);
    subject[sizeof(subject) - 1] = '\0';
    char line[384];
    snprintf(line, sizeof(line), "S 9 %s $default\n", subject);
    ASSERT_EQ(write_wal_line(line), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19992;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_malformed_numeric_id) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("U -1\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19991;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_truncated_and_unknown_records) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("X 1\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19990;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);

    char long_line[1100];
    memset(long_line, 'a', sizeof(long_line) - 2);
    long_line[0] = 'S';
    long_line[1] = ' ';
    long_line[sizeof(long_line) - 2] = '\n';
    long_line[sizeof(long_line) - 1] = '\0';
    ASSERT_EQ(write_wal_line(long_line), 0);
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_extra_fields_in_sub_record) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("S 9 valid.subject account unexpected_extra\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19989;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_tab_extra_fields_in_sub_record) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("S 9 valid.subject account\textra\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19988;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST_MAIN()
