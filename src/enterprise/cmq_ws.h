#ifndef CMQ_WS_H
#define CMQ_WS_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_WS_MAX_CLIENTS 128
#define CMQ_WS_KEY_SIZE    32
#define CMQ_WS_MAGIC_SIZE  28

typedef struct cmq_ws_server cmq_ws_server_t;
typedef struct cmq_ws_frame cmq_ws_frame_t;
typedef struct cmq_ws_client cmq_ws_client_t;

typedef enum {
    CMQ_WS_OPCODE_TEXT   = 0x01,
    CMQ_WS_OPCODE_BINARY = 0x02,
    CMQ_WS_OPCODE_CLOSE  = 0x08,
    CMQ_WS_OPCODE_PING   = 0x09,
    CMQ_WS_OPCODE_PONG   = 0x0A
} cmq_ws_opcode_t;

struct cmq_ws_frame {
    cmq_ws_opcode_t opcode;
    int fin;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t mask_key;
    int masked;
};

typedef void (*cmq_ws_on_message_cb)(void *ctx, int client_fd,
                                      const uint8_t *data, size_t len,
                                      cmq_ws_opcode_t opcode);

cmq_ws_server_t *cmq_ws_server_create(int port);
void cmq_ws_server_destroy(cmq_ws_server_t *srv);

int cmq_ws_server_start(cmq_ws_server_t *srv);
int cmq_ws_server_stop(cmq_ws_server_t *srv);
size_t cmq_ws_server_client_count(cmq_ws_server_t *srv);

void cmq_ws_server_set_callback(cmq_ws_server_t *srv,
                                 cmq_ws_on_message_cb cb, void *ctx);

int cmq_ws_frame_parse(const uint8_t *buf, size_t buf_len,
                        cmq_ws_frame_t *out_frame);
int cmq_ws_frame_serialize(const cmq_ws_frame_t *frame, uint8_t *buf,
                            size_t buf_len);

void cmq_ws_mask(uint8_t *data, size_t len, uint32_t mask_key);

int cmq_ws_send(int fd, const uint8_t *data, size_t len, cmq_ws_opcode_t opcode);
int cmq_ws_send_close(int fd, uint16_t code);

int cmq_ws_accept_key(const char *client_key, char *out_key, size_t out_len);
int cmq_ws_parse_http_upgrade(const char *req, size_t req_len,
                               char *ws_key_out, size_t key_len);
int cmq_ws_build_response(const char *accept_key, char *out, size_t out_len);

#endif
