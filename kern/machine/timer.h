/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/timer.h
 *
 * Architecture dispatch for the free-running counter API (timer_get_ticks,
 * timer_get_frequency, ...) that machine-independent code such as the
 * kernel console uses for timestamps.
 */

#ifndef NXU_KERN_MACHINE_TIMER_H
#define NXU_KERN_MACHINE_TIMER_H

#if defined(__aarch64__)
#include <kern/arm64/timer.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/timer.h>
#else
#error "machine/timer.h: unsupported target architecture"
#endif

#endif
