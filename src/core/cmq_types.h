#ifndef CMQ_TYPES_H
#define CMQ_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t cmq_u64_t;
typedef uint32_t cmq_u32_t;
typedef uint16_t cmq_u16_t;
typedef uint8_t  cmq_u8_t;
typedef int64_t  cmq_i64_t;
typedef int32_t  cmq_i32_t;
typedef int16_t  cmq_i16_t;
typedef int8_t   cmq_i8_t;

#define CMQ_ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define CMQ_ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define CMQ_IS_ALIGNED(x, align) (((x) & ((align) - 1)) == 0)
#define CMQ_LIKELY(x)   __builtin_expect(!!(x), 1)
#define CMQ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CMQ_UNUSED(x)   ((void)(x))
#define CMQ_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define CMQ_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CMQ_MIN(a, b) ((a) < (b) ? (a) : (b))

#endif
