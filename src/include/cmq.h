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


/* Default configuration */
#define CMQ_DEFAULT_PORT     7654
#define CMQ_DEFAULT_HOST     "0.0.0.0"
#define CMQ_DEFAULT_THREADS  0  /* 0 = auto-detect (number of CPU cores) */
#define CMQ_DEFAULT_MAX_CLIENTS  65536
/* Hard ceiling for max_clients / clients[] growth (prevents int wrap on *2). */
#define CMQ_MAX_CLIENTS_LIMIT    1000000
#define CMQ_DEFAULT_MAX_PAYLOAD  (1024 * 1024)  /* 1 MB */
/* Ceiling: encoded MESSAGE (+ WS binary header ≤10) must fit write_buf 4MiB. */
#define CMQ_MAX_PAYLOAD_LIMIT \
    ((4 * 1024 * 1024) - (256 * 2 + 65536 + 64 + 10))
#define CMQ_DEFAULT_MAX_SUBS_PER_CLIENT 1024
#define CMQ_RATE_LIMIT_SLOTS 1024
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
    int max_subs_per_client;       /* Max subscriptions per client */
    int ping_interval_ms;          /* Ping interval in milliseconds */
    int write_timeout_ms;          /* Write timeout in milliseconds */
    const char *config_file;       /* Path to config file (optional) */
    const char *log_file;          /* Path to log file (optional) */
    int log_level;                 /* Log level: 0=trace,1=debug,2=info,3=warn,4=error,5=fatal */
    int log_to_stdout;             /* Log to stdout (default: 1) */
    int log_to_file;               /* Log to file (default: 0) */
    const char *auth_username;
    const char *auth_password;
    const char *cluster_name;
    const char *cluster_node_id;
    struct { const char *addr; int port; } routes[8];
    int route_count;
    /* F14 quota knobs. 0 disables each individual cap. */
    int max_msgs_per_sec_per_account;
    int max_bytes_per_sec_per_account;
    int max_connections_per_account;
    /* F16 ACL: CSV patterns. NULL disables. */
    const char *acl_allow;
    const char *acl_deny;
    /* F15: connection blocklist file. NULL disables. */
    const char *blocklist_file;
    int tls_enabled;
    const char *tls_cert;
    const char *tls_key;
    int max_connects_per_sec;  /* F10: per-IP connect rate cap; 0=disabled */
    int inbox_max_pending;     /* F15: per-conn REQUEST pending cap; 0=disabled */
    const char *persist_dir;   /* F5: WAL directory; NULL = disabled */
    /* F6: MQTT bridge (client mode). When mqtt_bridge_addr is set,
     * the server forwards published messages matching a subject
     * mapping to the upstream broker. NULL = disabled. */
    const char *mqtt_bridge_addr;
    int mqtt_bridge_port;
} cmq_config_t;

/**
 * Create a new server with the given configuration.
 * Returns CMQ_OK on success, error code on failure.
 */
cmq_status_t cmq_server_create(cmq_server_t **server, const cmq_config_t *config);

/* N2: Reload dynamic config from a config file. Updates
 * blocklist_file, audit path, log levels, and similar dynamic
 * fields. Returns 0 on success, -1 on failure. */
int cmq_server_reload(cmq_server_t *server, const char *config_path);

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
 * Gracefully drain all connections and stop the server.
 * Sends DISCONNECT to all clients, waits up to drain_timeout_ms for completion.
 */
void cmq_server_drain(cmq_server_t *server, int drain_timeout_ms);

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
