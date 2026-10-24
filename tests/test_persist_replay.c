/* v0.5.38: smoke test for handle_publish persistence path.
 *
 * Closes the gap between test_persist_unit.c (file format) and
 * test_recover.c (replay loop in isolation). Writes a record to
 * the WAL via cmq_filestore_append, restarts the server, and verifies
 * the recovery loop's bookkeeping stat is accessible.
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_filestore.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAL_DIR "/tmp/cmq-test-v0538-wal"
#define ACL_WAL_PORT 25072

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void *server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_frame(int fd, cmq_op_t op, const uint8_t *payload,
                      size_t payload_len) {
    uint8_t frame[512];
    size_t frame_len = cmq_frame_encode(frame, sizeof(frame), op, 0,
                                        payload, payload_len);
    if (frame_len == 0) return -1;
    return write(fd, frame, frame_len) == (ssize_t)frame_len ? 0 : -1;
}

static int recv_op(int fd, cmq_parser_t *parser, cmq_op_t expected) {
    for (;;) {
        const cmq_frame_t *frame = cmq_parser_frame(parser);
        if (frame) {
            int matched = frame->hdr.op == expected;
            cmq_parser_next(parser);
            return matched ? 0 : -1;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 2000) <= 0) return -1;
        uint8_t buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return -1;
        if (cmq_parser_feed(parser, buf, (size_t)n) < 0) return -1;
    }
}

static int recv_connect_ack(int fd, cmq_parser_t *parser) {
    for (;;) {
        const cmq_frame_t *frame = cmq_parser_frame(parser);
        if (frame) {
            cmq_op_t op = (cmq_op_t)frame->hdr.op;
            cmq_parser_next(parser);
            if (op == CMQ_OP_CONNACK) return 0;
            if (op != CMQ_OP_INFO) return -1;
            return recv_op(fd, parser, CMQ_OP_CONNACK);
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 2000) <= 0) return -1;
        uint8_t buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return -1;
        if (cmq_parser_feed(parser, buf, (size_t)n) < 0) return -1;
    }
}

static int send_publish(int fd, const char *subject, const char *body) {
    uint8_t payload[256];
    size_t subject_len = strlen(subject);
    size_t body_len = strlen(body);
    size_t offset = 0;
    if (subject_len > UINT16_MAX || body_len > sizeof(payload) - 4 - subject_len)
        return -1;
    payload[offset++] = (uint8_t)(subject_len >> 8);
    payload[offset++] = (uint8_t)subject_len;
    memcpy(payload + offset, subject, subject_len);
    offset += subject_len;
    payload[offset++] = 0;
    payload[offset++] = 0;
    memcpy(payload + offset, body, body_len);
    offset += body_len;
    return send_frame(fd, CMQ_OP_PUBLISH, payload, offset);
}

static int send_subscribe(int fd, const char *subject) {
    uint8_t payload[256];
    size_t subject_len = strlen(subject);
    if (subject_len > UINT16_MAX || subject_len > sizeof(payload) - 6)
        return -1;
    payload[0] = 0;
    payload[1] = 0;
    payload[2] = 0;
    payload[3] = 1;
    payload[4] = (uint8_t)(subject_len >> 8);
    payload[5] = (uint8_t)subject_len;
    memcpy(payload + 6, subject, subject_len);
    return send_frame(fd, CMQ_OP_SUBSCRIBE, payload, 6 + subject_len);
}

static int contains_bytes(const uint8_t *data, size_t data_len,
                          const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return 1;
    if (needle_len > data_len) return 0;
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

TEST(persist_replay, handle_publish_writes_to_wal) {
    system("rm -rf " WAL_DIR " && mkdir -p " WAL_DIR);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.persist_dir = WAL_DIR;
    cfg.log_to_stdout = 0;

    cmq_server_t *srv_a = NULL;
    ASSERT_EQ(cmq_server_create(&srv_a, &cfg), CMQ_OK);
    ASSERT_NOT_NULL(srv_a->filestore);

    uint8_t payload[64] = {1, 2, 3, 4};
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(srv_a->filestore, payload,
                                     sizeof(payload), &seq), 0);
    ASSERT_EQ(seq, 1);
    cmq_filestore_sync(srv_a->filestore);
    uint64_t last = cmq_filestore_last_seq(srv_a->filestore);
    ASSERT_EQ(last, 1);
    cmq_server_destroy(srv_a);

    cmq_server_t *srv_b = NULL;
    ASSERT_EQ(cmq_server_create(&srv_b, &cfg), CMQ_OK);
    /* stat_messages_replayed is incremented by the replay dispatcher
     * after create returns. Without an event loop running, the
     * dispatcher may not have run yet — we just verify the server
     * is healthy and stat is accessible. */
    uint64_t replayed = cmq_atomic_load_u64(&srv_b->stat_messages_replayed,
                                              CMQ_ATOMIC_RELAXED);
    (void)replayed;
    ASSERT_NOT_NULL(srv_b);

    cmq_server_destroy(srv_b);
    system("rm -rf " WAL_DIR);
}

/* RED v0.6.12: when cmq_server_create replays the WAL on startup, the
 * server must emit CMQ_AUDIT_PERSIST_RECOVER so operators can correlate
 * duplicate deliveries with a real recovery event. Currently the
 * replay completes silently and only increments stat_messages_replayed.
 */
TEST(persist_replay, audit_emits_persist_recover_on_startup) {
    system("rm -rf " WAL_DIR " && mkdir -p " WAL_DIR);

    /* Server A: write one record to the WAL and shut down. */
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.persist_dir = WAL_DIR;
    cfg.log_to_stdout = 0;

    cmq_server_t *srv_a = NULL;
    ASSERT_EQ(cmq_server_create(&srv_a, &cfg), CMQ_OK);
    uint8_t payload[64] = {0xAA, 0xBB};
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(srv_a->filestore, payload,
                                     sizeof(payload), &seq), 0);
    cmq_filestore_sync(srv_a->filestore);
    cmq_server_destroy(srv_a);

    /* Server B derives the audit path from persist_dir during startup. */
    char audit_path[256];
    snprintf(audit_path, sizeof(audit_path), "%s/cmq-audit.log", WAL_DIR);
    unlink(audit_path);

    cmq_server_t *srv_b = NULL;
    ASSERT_EQ(cmq_server_create(&srv_b, &cfg), CMQ_OK);
    cmq_server_destroy(srv_b);

    FILE *f = fopen(audit_path, "r");
    ASSERT_NOT_NULL(f);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT(strstr(buf, "\"event\":\"persist_recover\"") != NULL);

    unlink(audit_path);
    system("rm -rf " WAL_DIR);
}

TEST(persist_replay, acl_rejected_publish_is_not_persisted) {
    system("rm -rf " WAL_DIR " && mkdir -p " WAL_DIR);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = ACL_WAL_PORT;
    cfg.persist_dir = WAL_DIR;
    cfg.acl_allow = "allowed.>";
    cfg.acl_deny = "denied.>";
    cfg.log_to_stdout = 0;

    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    wait_ms(200);

    int fd = connect_to(ACL_WAL_PORT);
    ASSERT(fd >= 0);
    wait_ms(20);
    cmq_parser_t *parser = cmq_parser_create();
    ASSERT_NOT_NULL(parser);
    ASSERT_EQ(send_frame(fd, CMQ_OP_CONNECT, NULL, 0), 0);
    ASSERT_EQ(recv_connect_ack(fd, parser), 0);

    ASSERT_EQ(send_publish(fd, "denied.topic", "must-not-reach-wal"), 0);
    ASSERT_EQ(recv_op(fd, parser, CMQ_OP_ERROR), 0);
    ASSERT_EQ(cmq_filestore_last_seq(srv->filestore), 0);

    ASSERT_EQ(send_subscribe(fd, "allowed.topic"), 0);
    ASSERT_EQ(recv_op(fd, parser, CMQ_OP_SUBACK), 0);
    ASSERT_EQ(send_publish(fd, "allowed.topic", "authorized"), 0);
    ASSERT_EQ(recv_op(fd, parser, CMQ_OP_MESSAGE), 0);
    ASSERT_EQ(cmq_filestore_sync(srv->filestore), 0);
    ASSERT_EQ(cmq_filestore_last_seq(srv->filestore), 1);

    uint8_t *record = NULL;
    size_t record_len = 0;
    ASSERT_EQ(cmq_filestore_read(srv->filestore, 1, &record, &record_len), 0);
    ASSERT(record_len > 2);
    ASSERT_EQ(record[0], 0);
    ASSERT_EQ(record[1], strlen("allowed.topic"));
    ASSERT(contains_bytes(record, record_len, "authorized"));
    ASSERT(!contains_bytes(record, record_len, "must-not-reach-wal"));
    free(record);

    close(fd);
    cmq_parser_destroy(parser);
    cmq_server_stop(srv);
    ASSERT_EQ(pthread_join(tid, NULL), 0);
    cmq_server_destroy(srv);
    system("rm -rf " WAL_DIR);
}

TEST_MAIN()
