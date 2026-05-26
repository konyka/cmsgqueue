#ifndef CMQ_PROTO_H
#define CMQ_PROTO_H

#include <stdint.h>
#include "cmq_types.h"

#define CMQ_PROTO_MAGIC_0  0xCA
#define CMQ_PROTO_MAGIC_1  0xFE
#ifndef CMQ_PROTO_VERSION
#define CMQ_PROTO_VERSION  0x01
#endif
#define CMQ_PROTO_HDR_SIZE 8

#define CMQ_FLAG_COMPRESSED 0x01
#define CMQ_FLAG_CHECKSUM   0x02
#define CMQ_FLAG_HEADERS    0x04
#define CMQ_FLAG_BATCH      0x08

typedef enum {
    CMQ_OP_CONNECT     = 0x01,
    CMQ_OP_CONNACK     = 0x02,
    CMQ_OP_PUBLISH     = 0x03,
    CMQ_OP_PUBACK      = 0x04,
    CMQ_OP_SUBSCRIBE   = 0x05,
    CMQ_OP_SUBACK      = 0x06,
    CMQ_OP_UNSUBSCRIBE = 0x07,
    CMQ_OP_UNSUBACK    = 0x08,
    CMQ_OP_MESSAGE     = 0x09,
    CMQ_OP_PING        = 0x0A,
    CMQ_OP_PONG        = 0x0B,
    CMQ_OP_DISCONNECT  = 0x0C,
    CMQ_OP_ERROR       = 0x0D,
    CMQ_OP_INFO        = 0x0E
} cmq_op_t;

typedef struct __attribute__((packed)) {
    cmq_u8_t  magic[2];
    cmq_u8_t  version;
    cmq_u8_t  flags;
    cmq_u8_t  op;
    cmq_u32_t length;
} cmq_frame_hdr_t;

#endif
