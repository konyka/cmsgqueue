/* v0.5.168: reload creates the log when create left it NULL. */
#include "cmq_log.h"
#include "cmq_test.h"

TEST(laa, apply) {
    cmq_log_t *log = NULL;
    ASSERT(cmq_log_reload_sinks(log, 1, NULL, 0) != 0);
    ASSERT_EQ(cmq_log_reload_attach(&log, CMQ_LOG_WARN), 0);
    ASSERT_NOT_NULL(log);
    ASSERT_EQ(cmq_log_get_level(log), CMQ_LOG_WARN);
    ASSERT_EQ(cmq_log_reload_sinks(log, 1, NULL, 0), 0);
    ASSERT_EQ(cmq_log_has_stdout(log), 1);
    cmq_log_t *same = log;
    ASSERT_EQ(cmq_log_reload_attach(&log, CMQ_LOG_TRACE), 0);
    ASSERT(log == same);
    ASSERT_EQ(cmq_log_get_level(log), CMQ_LOG_WARN);
    cmq_log_destroy(log);
}

TEST(laa, omitted) {
    cmq_log_t *log = NULL;
    ASSERT_EQ(cmq_log_reload_attach(&log, CMQ_LOG_INFO), 0);
    ASSERT_NOT_NULL(log);
    ASSERT_EQ(cmq_log_get_level(log), CMQ_LOG_INFO);
    cmq_log_destroy(log);
}

TEST(laa, empty) {
    cmq_log_t *log = cmq_log_create(CMQ_LOG_ERROR);
    ASSERT_NOT_NULL(log);
    cmq_log_t *same = log;
    ASSERT_EQ(cmq_log_reload_attach(&log, CMQ_LOG_DEBUG), 0);
    ASSERT(log == same);
    ASSERT_EQ(cmq_log_get_level(log), CMQ_LOG_ERROR);
    cmq_log_destroy(log);
}

TEST(laa, reject) {
    ASSERT(cmq_log_reload_attach(NULL, CMQ_LOG_INFO) != 0);
    cmq_log_t *log = NULL;
    ASSERT(cmq_log_reload_attach(&log, -1) != 0);
    ASSERT(log == NULL);
    ASSERT(cmq_log_reload_attach(&log, 6) != 0);
    ASSERT(log == NULL);
}

TEST_MAIN()
