#include "cmq_log.h"
#include "cmq_test.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static char log_buffer[4096];
static size_t log_buffer_pos = 0;

static void test_appender(const char *msg, size_t len, void *ctx) {
    (void)ctx;
    if (log_buffer_pos + len < sizeof(log_buffer)) {
        memcpy(log_buffer + log_buffer_pos, msg, len);
        log_buffer_pos += len;
    }
}

TEST(log, create_destroy) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    ASSERT_NOT_NULL(log);
    cmq_log_destroy(log);
}

TEST(log, log_levels) {
    ASSERT_EQ(CMQ_LOG_TRACE, 0);
    ASSERT_EQ(CMQ_LOG_DEBUG, 1);
    ASSERT_EQ(CMQ_LOG_INFO, 2);
    ASSERT_EQ(CMQ_LOG_WARN, 3);
    ASSERT_EQ(CMQ_LOG_ERROR, 4);
    ASSERT_EQ(CMQ_LOG_FATAL, 5);
}

TEST(log, filtering) {
    memset(log_buffer, 0, sizeof(log_buffer));
    log_buffer_pos = 0;

    cmq_log_t *log = cmq_log_create(CMQ_LOG_WARN);
    cmq_log_add_appender(log, test_appender, NULL);

    cmq_log_write(log, CMQ_LOG_DEBUG, "file.c", 10, "should not appear");
    ASSERT_EQ(log_buffer_pos, (size_t)0);

    cmq_log_write(log, CMQ_LOG_ERROR, "file.c", 20, "error msg");
    ASSERT(log_buffer_pos > 0);
    ASSERT(strstr(log_buffer, "error msg") != NULL);

    cmq_log_destroy(log);
}

TEST(log, format_output) {
    memset(log_buffer, 0, sizeof(log_buffer));
    log_buffer_pos = 0;

    cmq_log_t *log = cmq_log_create(CMQ_LOG_TRACE);
    cmq_log_add_appender(log, test_appender, NULL);

    cmq_log_write(log, CMQ_LOG_INFO, "test.c", 42, "hello %s %d", "world", 123);
    ASSERT(strstr(log_buffer, "hello world 123") != NULL);
    ASSERT(strstr(log_buffer, "test.c:42") != NULL);

    cmq_log_destroy(log);
}

TEST(log, file_appender) {
    const char *path = "/tmp/cmq_test_log.txt";
    remove(path);

    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    cmq_log_add_file(log, path);
    cmq_log_write(log, CMQ_LOG_INFO, "test.c", 1, "file test");
    cmq_log_flush(log);
    cmq_log_destroy(log);

    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    char buf[256];
    fgets(buf, sizeof(buf), f);
    fclose(f);
    ASSERT(strstr(buf, "file test") != NULL);
    remove(path);
}

TEST_MAIN()
