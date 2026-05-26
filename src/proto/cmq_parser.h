#ifndef CMQ_PARSER_H
#define CMQ_PARSER_H

#include <stdint.h>
#include <stddef.h>

#include "cmq_proto.h"
#include "cmq_types.h"

typedef struct cmq_parser cmq_parser_t;

typedef struct {
    cmq_frame_hdr_t hdr;
    uint8_t *payload;
    size_t payload_len;
} cmq_frame_t;

cmq_parser_t *cmq_parser_create(void);
void cmq_parser_destroy(cmq_parser_t *p);
void cmq_parser_reset(cmq_parser_t *p);

int cmq_parser_feed(cmq_parser_t *p, const uint8_t *data, size_t len);
const cmq_frame_t *cmq_parser_frame(cmq_parser_t *p);
int cmq_parser_next(cmq_parser_t *p);

size_t cmq_frame_encode(uint8_t *buf, size_t buf_size, cmq_op_t op, cmq_u8_t flags, const uint8_t *payload, size_t payload_len);

#endif
