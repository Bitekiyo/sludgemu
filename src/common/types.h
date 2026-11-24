#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;
typedef uintptr_t uptr;
typedef intptr_t sptr;
typedef size_t usize;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define KiB(x) ((u64)(x) << 10)
#define MiB(x) ((u64)(x) << 20)
#define GiB(x) ((u64)(x) << 30)

#define ALIGN_UP(x, a) (((x) + ((a)-1)) & ~((a)-1))
#define ALIGN_DOWN(x, a) ((x) & ~((a)-1))
