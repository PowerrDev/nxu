#ifndef NXU_STDINT_H
#define NXU_STDINT_H

#define UINT8_MAX  255U
#define UINT16_MAX 65535U
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

/*
 * The 64-bit and pointer-sized types follow the target's data model, the way
 * XNU's machine/types.h does: LP64 targets (arm64, x86_64) have a 64-bit
 * long, while ILP32 targets (i386) need long long for 64 bits and a plain
 * int for pointers. Keeping `long` for LP64 leaves every existing %lu/%ld
 * format string valid on the targets that already build.
 */
#if defined(__LP64__)
typedef signed long int64_t;
typedef unsigned long uint64_t;

typedef signed long intptr_t;
typedef unsigned long uintptr_t;
#else
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

typedef signed int intptr_t;
typedef unsigned int uintptr_t;
#endif

#endif
