#define _POSIX_C_SOURCE 200809L
#include "cmq_ws.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct cmq_ws_client {
    int fd;
    int active;
    uint8_t *recv_buf;
    size_t recv_len;
    size_t recv_cap;
};

struct cmq_ws_server {
    int port;
    int listen_fd;
    int running;
    cmq_ws_client_t clients[CMQ_WS_MAX_CLIENTS];
    size_t client_count;
    cmq_ws_on_message_cb on_msg;
    void *on_msg_ctx;
};

cmq_ws_server_t *cmq_ws_server_create(int port) {
    cmq_ws_server_t *srv = calloc(1, sizeof(cmq_ws_server_t));
    if (!srv) return NULL;
    srv->port = port;
    srv->listen_fd = -1;
    srv->running = 0;
    return srv;
}

void cmq_ws_server_destroy(cmq_ws_server_t *srv) {
    if (!srv) return;
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    for (size_t i = 0; i < srv->client_count; i++) {
        if (srv->clients[i].fd >= 0) close(srv->clients[i].fd);
        free(srv->clients[i].recv_buf);
    }
    free(srv);
}

int cmq_ws_server_start(cmq_ws_server_t *srv) {
    if (!srv) return -1;
    srv->running = 1;
    return 0;
}

int cmq_ws_server_stop(cmq_ws_server_t *srv) {
    if (!srv) return -1;
    srv->running = 0;
    return 0;
}

size_t cmq_ws_server_client_count(cmq_ws_server_t *srv) {
    return srv ? srv->client_count : 0;
}

void cmq_ws_server_set_callback(cmq_ws_server_t *srv,
                                 cmq_ws_on_message_cb cb, void *ctx) {
    if (!srv) return;
    srv->on_msg = cb;
    srv->on_msg_ctx = ctx;
}

int cmq_ws_frame_parse(const uint8_t *buf, size_t buf_len,
                        cmq_ws_frame_t *out_frame) {
    if (!buf || !out_frame || buf_len < 2) return -1;

    out_frame->fin = (buf[0] >> 7) & 0x01;
    out_frame->opcode = (cmq_ws_opcode_t)(buf[0] & 0x0F);
    out_frame->masked = (buf[1] >> 7) & 0x01;

    uint64_t payload_len = (uint64_t)(buf[1] & 0x7F);
    size_t header_len = 2;

    if (payload_len == 126) {
        if (buf_len < 4) return -1;
        payload_len = ((uint64_t)buf[2] << 8) | buf[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (buf_len < 10) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | buf[2 + i];
        header_len = 10;
    }

    if (out_frame->masked) {
        if (buf_len < header_len + 4) return -1;
        out_frame->mask_key = ((uint32_t)buf[header_len] << 24) |
                              ((uint32_t)buf[header_len + 1] << 16) |
                              ((uint32_t)buf[header_len + 2] << 8) |
                              buf[header_len + 3];
        header_len += 4;
    }

    if (buf_len < header_len + (size_t)payload_len) return -1;

    out_frame->payload_len = (size_t)payload_len;
    out_frame->payload = (uint8_t *)&buf[header_len];
    return (int)(header_len + (size_t)payload_len);
}

int cmq_ws_frame_serialize(const cmq_ws_frame_t *frame, uint8_t *buf,
                            size_t buf_len) {
    if (!frame || !buf) return -1;

    size_t header_len = 2;
    if (frame->payload_len <= 125) {
        header_len = 2;
    } else if (frame->payload_len <= 65535) {
        header_len = 4;
    } else {
        header_len = 10;
    }

    size_t total = header_len + frame->payload_len;
    if (total > buf_len) return -1;

    buf[0] = (uint8_t)((frame->fin ? 0x80 : 0x00) | (frame->opcode & 0x0F));

    if (frame->payload_len <= 125) {
        buf[1] = (uint8_t)frame->payload_len;
    } else if (frame->payload_len <= 65535) {
        buf[1] = 126;
        buf[2] = (uint8_t)((frame->payload_len >> 8) & 0xFF);
        buf[3] = (uint8_t)(frame->payload_len & 0xFF);
    } else {
        buf[1] = 127;
        for (int i = 0; i < 8; i++)
            buf[2 + i] = (uint8_t)((frame->payload_len >> (56 - i * 8)) & 0xFF);
    }

    if (frame->payload && frame->payload_len > 0)
        memcpy(&buf[header_len], frame->payload, frame->payload_len);

    return (int)total;
}

void cmq_ws_mask(uint8_t *data, size_t len, uint32_t mask_key) {
    if (!data) return;
    uint8_t mk[4];
    mk[0] = (uint8_t)(mask_key >> 24);
    mk[1] = (uint8_t)(mask_key >> 16);
    mk[2] = (uint8_t)(mask_key >> 8);
    mk[3] = (uint8_t)(mask_key);
    for (size_t i = 0; i < len; i++)
        data[i] ^= mk[i % 4];
}

int cmq_ws_send(int fd, const uint8_t *data, size_t len, cmq_ws_opcode_t opcode) {
    if (fd < 0 || (!data && len > 0)) return -1;

    cmq_ws_frame_t frame = {0};
    frame.fin = 1;
    frame.opcode = opcode;
    frame.payload = (uint8_t *)data;
    frame.payload_len = len;
    frame.masked = 0;

    uint8_t hdr[10];
    int total = cmq_ws_frame_serialize(&frame, hdr, sizeof(hdr));
    if (total < 0) return -1;

    size_t hdr_len = (size_t)total - len;
    if (write(fd, hdr, hdr_len) < 0) return -1;
    if (len > 0 && write(fd, data, len) < 0) return -1;
    return 0;
}

int cmq_ws_send_close(int fd, uint16_t code) {
    uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
    return cmq_ws_send(fd, payload, 2, CMQ_WS_OPCODE_CLOSE);
}

int cmq_ws_accept_key(const char *client_key, char *out_key, size_t out_len) {
    if (!client_key || !out_key || out_len < 29) return -1;

    size_t key_len = strlen(client_key);
    size_t combined_len = key_len + sizeof(WS_MAGIC) - 1;
    uint8_t *combined = malloc(combined_len);
    if (!combined) return -1;

    memcpy(combined, client_key, key_len);
    memcpy(combined + key_len, WS_MAGIC, sizeof(WS_MAGIC) - 1);

    unsigned char hash[SHA_DIGEST_LENGTH];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { free(combined); return -1; }
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, combined, combined_len);
    EVP_DigestFinal_ex(ctx, hash, NULL);
    EVP_MD_CTX_free(ctx);
    free(combined);

    size_t b64_len = 0;
    EVP_EncodeBlock((unsigned char *)out_key, hash, SHA_DIGEST_LENGTH);
    (void)b64_len;
    return 0;
}

int cmq_ws_parse_http_upgrade(const char *req, size_t req_len,
                               char *ws_key_out, size_t key_len) {
    if (!req || !ws_key_out) return -1;
    const char *marker = "Sec-WebSocket-Key: ";
    const char *pos = strstr(req, marker);
    if (!pos) return -1;
    pos += strlen(marker);
    const char *end = strchr(pos, '\r');
    if (!end) end = strchr(pos, '\n');
    if (!end) return -1;
    size_t klen = (size_t)(end - pos);
    if (klen >= key_len) klen = key_len - 1;
    memcpy(ws_key_out, pos, klen);
    ws_key_out[klen] = '\0';
    return 0;
}

int cmq_ws_build_response(const char *accept_key, char *out, size_t out_len) {
    if (!accept_key || !out) return -1;
    return snprintf(out, out_len,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key) > 0 ? 0 : -1;
}
