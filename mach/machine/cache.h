/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/machine/cache.h
 *
 * Architecture dispatch for CPU cache maintenance, as XNU's <machine/cache.h>
 * does. Machine-independent code that writes instructions into memory it
 * then executes (the ELF loader is the one caller today) includes this header
 * and calls cache_sync_instruction_range(); it never names an architecture.
 *
 *   arm64   forwards to <mach/arm64/cache.h>: the full cache bring-up and
 *           maintenance API, unchanged.
 *   i386    <mach/i386/cache.h>: the instruction and data caches are
 *           coherent, so the maintenance routines are no-ops.
 */

#ifndef NXU_MACH_MACHINE_CACHE_H
#define NXU_MACH_MACHINE_CACHE_H

#if defined(__aarch64__)
#include <mach/arm64/cache.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <mach/i386/cache.h>
#else
#error "machine/cache.h: unsupported target architecture"
#endif

#endif
