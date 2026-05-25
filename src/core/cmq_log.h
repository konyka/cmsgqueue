#ifndef CMQ_LOG_H
#define CMQ_LOG_H

#include <stddef.h>

typedef struct cmq_log cmq_log_t;

typedef enum {
    CMQ_LOG_TRACE = 0,
    CMQ_LOG_DEBUG = 1,
    CMQ_LOG_INFO  = 2,
    CMQ_LOG_WARN  = 3,
    CMQ_LOG_ERROR = 4,
    CMQ_LOG_FATAL = 5
} cmq_log_level_t;

typedef void (*cmq_log_appender_fn)(const char *msg, size_t len, void *ctx);

cmq_log_t *cmq_log_create(cmq_log_level_t level);
void cmq_log_destroy(cmq_log_t *log);
void cmq_log_set_level(cmq_log_t *log, cmq_log_level_t level);
void cmq_log_add_appender(cmq_log_t *log, cmq_log_appender_fn fn, void *ctx);
void cmq_log_add_file(cmq_log_t *log, const char *path);
void cmq_log_add_stdout(cmq_log_t *log);
void cmq_log_write(cmq_log_t *log, cmq_log_level_t level, const char *file, int line, const char *fmt, ...);
void cmq_log_flush(cmq_log_t *log);

#define cmq_log_trace(log, fmt, ...) cmq_log_write(log, CMQ_LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define cmq_log_debug(log, fmt, ...) cmq_log_write(log, CMQ_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define cmq_log_info(log, fmt, ...)  cmq_log_write(log, CMQ_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define cmq_log_warn(log, fmt, ...)  cmq_log_write(log, CMQ_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define cmq_log_error(log, fmt, ...) cmq_log_write(log, CMQ_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define cmq_log_fatal(log, fmt, ...) cmq_log_write(log, CMQ_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif
