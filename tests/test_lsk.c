/* v0.5.127: reload applies log sinks without duplicating appenders. */
#include "cmq_log.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>

TEST(lsk, apply) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    ASSERT(log != NULL);
    ASSERT_EQ(cmq_log_reload_sinks(log, 1, "/tmp/cmq_lsk1.log", 1), 0);
    ASSERT_EQ(cmq_log_has_stdout(log), 1);
    ASSERT_EQ(cmq_log_appender_count(log), (size_t)2);
    char path[256];
    ASSERT_EQ(cmq_log_file_path(log, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_lsk1.log");
    ASSERT_EQ(cmq_log_reload_sinks(log, 1, "/tmp/cmq_lsk1.log", 1), 0);
    ASSERT_EQ(cmq_log_appender_count(log), (size_t)2);
    ASSERT_EQ(cmq_log_reload_sinks(log, 1, "/tmp/cmq_lsk2.log", 1), 0);
    ASSERT_EQ(cmq_log_appender_count(log), (size_t)2);
    ASSERT_EQ(cmq_log_file_path(log, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_lsk2.log");
    cmq_log_destroy(log);
}

TEST(lsk, omitted) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    cmq_log_add_stdout(log);
    cmq_log_add_file(log, "/tmp/cmq_lsk_keep.log");
    ASSERT_EQ(cmq_log_reload_sinks(log, 0, NULL, 0), 0);
    ASSERT_EQ(cmq_log_has_stdout(log), 1);
    ASSERT_EQ(cmq_log_appender_count(log), (size_t)2);
    char path[256];
    ASSERT_EQ(cmq_log_file_path(log, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_lsk_keep.log");
    cmq_log_destroy(log);
}

TEST(lsk, empty) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    cmq_log_add_file(log, "/tmp/cmq_lsk_empty.log");
    ASSERT_EQ(cmq_log_reload_sinks(log, 0, "", 1), 0);
    char path[256];
    ASSERT_EQ(cmq_log_file_path(log, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_lsk_empty.log");
    cmq_log_destroy(log);
}

TEST(lsk, reject) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_INFO);
    cmq_log_add_file(log, "/tmp/cmq_lsk_rej.log");
    ASSERT(cmq_log_reload_sinks(log, 2, NULL, 0) != 0);
    ASSERT(cmq_log_reload_sinks(NULL, 1, NULL, 0) != 0);
    char path[256];
    ASSERT_EQ(cmq_log_file_path(log, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_lsk_rej.log");
    cmq_log_destroy(log);
}

TEST_MAIN()
