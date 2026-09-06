#define _POSIX_C_SOURCE 200809L
#include "cmq_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>


#define CMQ_LOG_MAX_APPENDERS 8
#define CMQ_LOG_MSG_SIZE 1024

/* Borrowed 32-char hex or NULL. Per-worker; no lock. */
static _Thread_local const char *g_log_trace;

static int trace_hex_ok(const char *hex) {
    if (!hex) return 0;
    size_t n = 0;
    while (n < 32 && hex[n]) {
        char c = hex[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
        n++;
    }
    return n == 32 && hex[32] == '\0';
}

void cmq_log_set_thread_trace(const char *hex) {
    g_log_trace = trace_hex_ok(hex) ? hex : NULL;
}

const char *cmq_log_thread_trace(void) {
    return g_log_trace;
}


typedef struct {
    cmq_log_appender_fn fn;
    void *ctx;
} cmq_log_appender_t;

struct cmq_log {
    cmq_log_level_t level;
    pthread_mutex_t lock;
    cmq_log_appender_t appenders[CMQ_LOG_MAX_APPENDERS];
    size_t appender_count;
    atomic_int in_flight; /* dispatch + set_level/add_appender vs destroy */
    atomic_int dying;     /* destroy claimed — no new dispatch */
};

static int log_begin_op(cmq_log_t *log) {
    if (atomic_load_explicit(&log->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&log->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&log->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void log_end_op(cmq_log_t *log) {
    atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
}


static const char* level_to_string(cmq_log_level_t level) {
    switch (level) {
        case CMQ_LOG_TRACE: return "TRACE";
        case CMQ_LOG_DEBUG: return "DEBUG";
        case CMQ_LOG_INFO:  return "INFO";
        case CMQ_LOG_WARN:  return "WARN";
        case CMQ_LOG_ERROR: return "ERROR";
        case CMQ_LOG_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}


static void cmq_log_file_appender(const char *msg, size_t len, void *ctx) {
    FILE *f = (FILE *)ctx;
    if (f) {
        /* Serialize vs cmq_log_flush fflush on the same FILE*. */
        flockfile(f);
        fwrite(msg, 1, len, f);
        funlockfile(f);
    }
}

static void cmq_log_stdout_appender(const char *msg, size_t len, void *ctx) {
    (void)ctx;
    /* Align file appender — multi-worker write races on stdout FILE*. */
    flockfile(stdout);
    fwrite(msg, 1, len, stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void cmq_format_time(char *out, size_t out_size) {
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* Caller must hold in_flight (and have checked !dying). */
static void cmq_dispatch_log(cmq_log_t *log, const char *line, size_t len) {
    // Take a snapshot of appenders under lock to avoid re-entrancy issues
    cmq_log_appender_t snapshot[CMQ_LOG_MAX_APPENDERS];
    size_t count = 0;
    if (!log) return;
    pthread_mutex_lock(&log->lock);
    count = log->appender_count;
    for (size_t i = 0; i < count; ++i) {
        snapshot[i] = log->appenders[i];
    }
    pthread_mutex_unlock(&log->lock);
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].fn) {
            snapshot[i].fn(line, len, snapshot[i].ctx);
        }
    }
}

cmq_log_t *cmq_log_create(cmq_log_level_t level) {
    cmq_log_t *log = (cmq_log_t *)malloc(sizeof(cmq_log_t));
    if (!log) return NULL;
    log->level = level;
    log->appender_count = 0;
    atomic_init(&log->in_flight, 0);
    atomic_init(&log->dying, 0);
    if (pthread_mutex_init(&log->lock, NULL) != 0) {
        free(log);
        return NULL;
    }
    return log;
}

void cmq_log_destroy(cmq_log_t *log) {
    if (!log) return;
    atomic_store_explicit(&log->dying, 1, memory_order_release);
    /* Wait for unlocked appender callbacks before fclose/free. */
    while (atomic_load_explicit(&log->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    // Close any file appenders
    pthread_mutex_lock(&log->lock);
    for (size_t i = 0; i < log->appender_count; ++i) {
        if (log->appenders[i].fn == cmq_log_file_appender) {
            FILE *f = (FILE *)log->appenders[i].ctx;
            if (f) fclose(f);
        }
    }
    pthread_mutex_unlock(&log->lock);
    pthread_mutex_destroy(&log->lock);
    free(log);
}

void cmq_log_set_level(cmq_log_t *log, cmq_log_level_t level) {
    if (!log) return;
    if (log_begin_op(log) != 0) return;
    pthread_mutex_lock(&log->lock);
    log->level = level;
    pthread_mutex_unlock(&log->lock);
    log_end_op(log);
}

int cmq_log_add_appender(cmq_log_t *log, cmq_log_appender_fn fn, void *ctx) {
    if (!log || !fn) return -1;
    if (log_begin_op(log) != 0) return -1;
    int rc = -1;
    pthread_mutex_lock(&log->lock);
    if (log->appender_count < CMQ_LOG_MAX_APPENDERS) {
        log->appenders[log->appender_count].fn = fn;
        log->appenders[log->appender_count].ctx = ctx;
        log->appender_count++;
        rc = 0;
    }
    pthread_mutex_unlock(&log->lock);
    log_end_op(log);
    return rc;
}

void cmq_log_add_file(cmq_log_t *log, const char *path) {
    if (!log || !path) return;
    /* Hold in_flight across fopen — destroy must not free(log) mid-open. */
    if (log_begin_op(log) != 0) return;
    FILE *f = fopen(path, "a");
    if (!f) {
        log_end_op(log);
        return;
    }
    int rc = -1;
    pthread_mutex_lock(&log->lock);
    if (log->appender_count < CMQ_LOG_MAX_APPENDERS) {
        log->appenders[log->appender_count].fn = cmq_log_file_appender;
        log->appenders[log->appender_count].ctx = f;
        log->appender_count++;
        rc = 0;
    }
    pthread_mutex_unlock(&log->lock);
    if (rc != 0)
        fclose(f);
    log_end_op(log);
}

void cmq_log_add_stdout(cmq_log_t *log) {
    if (!log) return;
    cmq_log_add_appender(log, cmq_log_stdout_appender, NULL);
}

void cmq_log_write(cmq_log_t *log, cmq_log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (!log || !fmt) return;
    if (atomic_load_explicit(&log->dying, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&log->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&log->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
        return;
    }
    int log_now = 0;
    pthread_mutex_lock(&log->lock);
    log_now = (level >= log->level);
    pthread_mutex_unlock(&log->lock);
    if (!log_now) {
        atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
        return;
    }

    char user_buf[CMQ_LOG_MSG_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(user_buf, CMQ_LOG_MSG_SIZE, fmt, ap);
    va_end(ap);

    char timebuf[20];
    cmq_format_time(timebuf, sizeof(timebuf));

    char final_buf[CMQ_LOG_MSG_SIZE];
    const char *lvlstr = level_to_string(level);
    const char *tid = g_log_trace;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    if (tid) {
        snprintf(final_buf, sizeof(final_buf),
                 "[%s] [%s] [tid=%s] %s:%d: %s",
                 lvlstr, timebuf, tid, file, line, user_buf);
    } else {
        snprintf(final_buf, sizeof(final_buf), "[%s] [%s] %s:%d: %s",
                 lvlstr, timebuf, file, line, user_buf);
    }
#pragma GCC diagnostic pop
    size_t flen = strlen(final_buf);
    if (flen + 1 < sizeof(final_buf)) {
        final_buf[flen] = '\n';
        final_buf[flen + 1] = '\0';
    }

    cmq_dispatch_log(log, final_buf, strlen(final_buf));
    atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
}

void cmq_log_flush(cmq_log_t *log) {
    if (!log) return;
    if (atomic_load_explicit(&log->dying, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&log->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&log->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
        return;
    }
    pthread_mutex_lock(&log->lock);
    size_t count = log->appender_count;
    for (size_t i = 0; i < count; ++i) {
        if (log->appenders[i].fn == cmq_log_file_appender) {
            FILE *f = (FILE *)log->appenders[i].ctx;
            if (f) {
                flockfile(f);
                fflush(f);
                funlockfile(f);
            }
        } else if (log->appenders[i].fn == cmq_log_stdout_appender) {
            flockfile(stdout);
            fflush(stdout);
            funlockfile(stdout);
        }
    }
    pthread_mutex_unlock(&log->lock);
    atomic_fetch_sub_explicit(&log->in_flight, 1, memory_order_acq_rel);
}
