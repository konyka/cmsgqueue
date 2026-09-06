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

/* Eagerly load the cert chain and private key into the SSL_CTX.
 * Returns 0 on success, -1 on failure (bad cert file, bad key, mismatch).
 * Must be called after the last cmq_tls_set_* call but before any
 * session creation. With OpenSSL this is implemented in tls_build_ssl_ctx;
 * without OpenSSL this is a no-op returning 0. */
int cmq_tls_load(cmq_tls_config_t *cfg);
int cmq_tls_set_ca(cmq_tls_config_t *cfg, const char *ca_path);
int cmq_tls_set_verify(cmq_tls_config_t *cfg, int verify_peer);
int cmq_tls_set_crl(cmq_tls_config_t *cfg, const char *crl_path);
int cmq_tls_crl_path(cmq_tls_config_t *cfg, char *out, size_t out_len);
int cmq_tls_set_server_name(cmq_tls_config_t *cfg, const char *name);

/* F12: ALPN protocol list. Comma-separated (e.g. "h2,http/1.1").
 * Set BEFORE cmq_tls_load. NULL clears. */
int cmq_tls_set_alpn(cmq_tls_config_t *cfg, const char *protos_csv);
int cmq_tls_alpn_has(cmq_tls_config_t *cfg, const char *proto);

/* F12: Reload the SSL_CTX from the current cert/key paths.
 * Atomically swaps; existing sessions continue with the old CTX
 * until they tear down. Returns 0 on success, -1 on failure. */
int cmq_tls_reload(cmq_tls_config_t *cfg);

/* Copy-out under begin_op — never return an interior pointer (destroy UAF). */
int cmq_tls_cert_path(cmq_tls_config_t *cfg, char *out, size_t out_len);
int cmq_tls_key_path(cmq_tls_config_t *cfg, char *out, size_t out_len);
int cmq_tls_ca_path(cmq_tls_config_t *cfg, char *out, size_t out_len);
int cmq_tls_verify_peer(cmq_tls_config_t *cfg);
int cmq_tls_server_name(cmq_tls_config_t *cfg, char *out, size_t out_len);
int cmq_tls_configured(cmq_tls_config_t *cfg);
/* 1 if linked against a real crypto backend; 0 for the plaintext stub. */
int cmq_tls_backend_secure(void);

cmq_tls_session_t *cmq_tls_server_session(cmq_tls_config_t *cfg, int fd);
cmq_tls_session_t *cmq_tls_client_session(cmq_tls_config_t *cfg, int fd);
void cmq_tls_session_destroy(cmq_tls_session_t *session);

int cmq_tls_handshake(cmq_tls_session_t *session);
ssize_t cmq_tls_read(cmq_tls_session_t *session, uint8_t *buf, size_t len);
ssize_t cmq_tls_write(cmq_tls_session_t *session, const uint8_t *buf, size_t len);
int cmq_tls_fd(cmq_tls_session_t *session);

/* v0.5.23: opaque accessors for the session-resumption cache.
 * The cache owns the slot lifetime; cmq_tls_session_free_slot frees
 * an SSL_SESSION* (or NULL safely). */
void *cmq_tls_get_session_cache_state(cmq_tls_config_t *cfg);
int cmq_tls_set_session_cache_state(cmq_tls_config_t *cfg, void *state);
void cmq_tls_session_free_slot(void *sess);

#ifdef CMQ_TLS_OPENSSL
#include <openssl/ssl.h>
/* v0.5.27: test-only accessor. Returns the SSL_CTX built by
 * cmq_tls_load so tests can verify the session cache mode and
 * callback registration. Do NOT use in production code. */
SSL_CTX *cmq_tls_get_ssl_ctx_for_test(cmq_tls_config_t *cfg);
#endif

#endif
