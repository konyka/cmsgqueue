#ifndef CMQ_MONITOR_H
#define CMQ_MONITOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v0.5.47: NATS-style introspection JSON. Snapshot structs are
 * filled by the server; formatters are allocation-free. */

#define CMQ_MONITOR_CONN_MAX  64
#define CMQ_MONITOR_SUB_MAX   256
#define CMQ_MONITOR_ROUTE_MAX 32
#define CMQ_MONITOR_JSON_MAX  8192

typedef struct cmq_monitor_conn {
    uint32_t cid;
    int state;              /* cmq_client_state_t */
    char account[64];
    char user[64];
    int nsubs;
    int ws;
    int route;
    char tid[33];
} cmq_monitor_conn_t;

typedef struct cmq_monitor_sub {
    uint32_t cid;
    uint32_t sid;
    char subject[256];
    char queue[64];
} cmq_monitor_sub_t;

typedef struct cmq_monitor_route {
    char id[16];
    char addr[64];
    int port;
    int connected;
    int fd;
} cmq_monitor_route_t;

int cmq_json_escape(const char *in, char *out, size_t cap);

int cmq_monitor_format_connz(char *buf, size_t cap,
                              const cmq_monitor_conn_t *conns, int n);
int cmq_monitor_format_subz(char *buf, size_t cap,
                             const cmq_monitor_sub_t *subs, int n);
int cmq_monitor_format_routez(char *buf, size_t cap,
                               int live, int held, int targets,
                               const cmq_monitor_route_t *routes, int n);

#ifdef __cplusplus
}
#endif

#endif
