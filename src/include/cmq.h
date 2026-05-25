/**
 * cmq.h - CMSGQueue Public API
 *
 * High-performance message queue in pure C.
 * Copyright (c) 2025 CMSGQueue Contributors
 * Licensed under the Apache License, Version 2.0
 */

#ifndef CMQ_H
#define CMQ_H

#ifdef __cplusplus
extern "C" {
#endif

/* Version information */
#define CMQ_VERSION_MAJOR 0
#define CMQ_VERSION_MINOR 1
#define CMQ_VERSION_PATCH 0
#define CMQ_VERSION_STRING "0.1.0"

/* Protocol version */
#define CMQ_PROTO_VERSION 1

/* Default configuration */
#define CMQ_DEFAULT_PORT     7654
#define CMQ_DEFAULT_HOST     "0.0.0.0"
#define CMQ_DEFAULT_THREADS  0  /* 0 = auto-detect (number of CPU cores) */
#define CMQ_DEFAULT_MAX_CLIENTS  65536
#define CMQ_DEFAULT_MAX_PAYLOAD  (1024 * 1024)  /* 1 MB */
#define CMQ_DEFAULT_PING_INTERVAL 30000  /* 30 seconds */
#define CMQ_DEFAULT_WRITE_TIMEOUT  5000  /* 5 seconds */

/* Return codes */
typedef enum cmq_status {
    CMQ_OK = 0,
    CMQ_ERR_INVALID_ARG = -1,
    CMQ_ERR_NO_MEMORY   = -2,
    CMQ_ERR_IO          = -3,
    CMQ_ERR_TIMEOUT     = -4,
    CMQ_ERR_DISCONNECTED = -5,
    CMQ_ERR_PROTOCOL    = -6,
    CMQ_ERR_AUTH        = -7,
    CMQ_ERR_NOT_FOUND   = -8,
    CMQ_ERR_EXISTS      = -9,
    CMQ_ERR_TOO_LARGE   = -10,
    CMQ_ERR_SHUTDOWN    = -11,
    CMQ_ERR_UNKNOWN     = -99
} cmq_status_t;

/* Opaque server handle */
typedef struct cmq_server cmq_server_t;

/**
 * Server configuration.
 * Zero-initialize and set fields as needed.
 */
typedef struct cmq_config {
    const char *host;              /* Bind host (default: "0.0.0.0") */
    int port;                      /* Bind port (default: 7654) */
    int num_threads;               /* Worker threads (default: auto) */
    int max_clients;               /* Max concurrent clients */
    int max_payload_size;          /* Max message payload in bytes */
    int ping_interval_ms;          /* Ping interval in milliseconds */
    int write_timeout_ms;          /* Write timeout in milliseconds */
    const char *config_file;       /* Path to config file (optional) */
    const char *log_file;          /* Path to log file (optional) */
    int log_level;                 /* Log level: 0=trace,1=debug,2=info,3=warn,4=error,5=fatal */
    int log_to_stdout;             /* Log to stdout (default: 1) */
    int log_to_file;               /* Log to file (default: 0) */
} cmq_config_t;

/**
 * Create a new server with the given configuration.
 * Returns CMQ_OK on success, error code on failure.
 */
cmq_status_t cmq_server_create(cmq_server_t **server, const cmq_config_t *config);

/**
 * Start the server (blocking).
 * Returns CMQ_OK on clean shutdown, error code on failure.
 */
cmq_status_t cmq_server_run(cmq_server_t *server);

/**
 * Signal the server to stop.
 * Safe to call from signal handlers.
 */
void cmq_server_stop(cmq_server_t *server);

/**
 * Destroy the server and free all resources.
 */
void cmq_server_destroy(cmq_server_t *server);

/**
 * Get server version string.
 */
const char *cmq_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_H */
