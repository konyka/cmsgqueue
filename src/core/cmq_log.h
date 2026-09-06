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
/* Returns 0 on success, -1 if full or invalid. */
int cmq_log_add_appender(cmq_log_t *log, cmq_log_appender_fn fn, void *ctx);
void cmq_log_add_file(cmq_log_t *log, const char *path);
void cmq_log_add_stdout(cmq_log_t *log);
void cmq_log_write(cmq_log_t *log, cmq_log_level_t level, const char *file, int line, const char *fmt, ...);
void cmq_log_flush(cmq_log_t *log);

/* v0.5.44: borrowed 32-char lowercase hex, or NULL to clear.
 * Junk / wrong length is ignored so log format stays injection-free. */
void cmq_log_set_thread_trace(const char *hex);
const char *cmq_log_thread_trace(void);

#define cmq_log_trace(log, ...) cmq_log_write(log, CMQ_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define cmq_log_debug(log, ...) cmq_log_write(log, CMQ_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define cmq_log_info(log, ...)  cmq_log_write(log, CMQ_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define cmq_log_warn(log, ...)  cmq_log_write(log, CMQ_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define cmq_log_error(log, ...) cmq_log_write(log, CMQ_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define cmq_log_fatal(log, ...) cmq_log_write(log, CMQ_LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif
