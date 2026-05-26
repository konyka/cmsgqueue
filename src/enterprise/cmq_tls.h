#ifndef CMQ_TLS_H
#define CMQ_TLS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct cmq_tls_config cmq_tls_config_t;
typedef struct cmq_tls_session cmq_tls_session_t;

cmq_tls_config_t *cmq_tls_config_create(void);
void cmq_tls_config_destroy(cmq_tls_config_t *cfg);

int cmq_tls_set_cert(cmq_tls_config_t *cfg, const char *cert_path);
int cmq_tls_set_key(cmq_tls_config_t *cfg, const char *key_path);
int cmq_tls_set_ca(cmq_tls_config_t *cfg, const char *ca_path);
int cmq_tls_set_verify(cmq_tls_config_t *cfg, int verify_peer);
int cmq_tls_set_server_name(cmq_tls_config_t *cfg, const char *name);

const char *cmq_tls_cert_path(cmq_tls_config_t *cfg);
const char *cmq_tls_key_path(cmq_tls_config_t *cfg);
const char *cmq_tls_ca_path(cmq_tls_config_t *cfg);
int cmq_tls_verify_peer(cmq_tls_config_t *cfg);
const char *cmq_tls_server_name(cmq_tls_config_t *cfg);
int cmq_tls_configured(cmq_tls_config_t *cfg);

cmq_tls_session_t *cmq_tls_server_session(cmq_tls_config_t *cfg, int fd);
cmq_tls_session_t *cmq_tls_client_session(cmq_tls_config_t *cfg, int fd);
void cmq_tls_session_destroy(cmq_tls_session_t *session);

int cmq_tls_handshake(cmq_tls_session_t *session);
ssize_t cmq_tls_read(cmq_tls_session_t *session, uint8_t *buf, size_t len);
ssize_t cmq_tls_write(cmq_tls_session_t *session, const uint8_t *buf, size_t len);
int cmq_tls_fd(cmq_tls_session_t *session);

#endif
