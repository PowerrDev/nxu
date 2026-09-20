/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/system.h
 *
 * Architecture dispatch for whole-machine power control, in the manner of
 * XNU's PE_halt_restart. Machine-independent code (the reset system call)
 * includes this header and never names the firmware interface.
 *
 *   machine_system_reset()      restart the machine
 *   machine_system_power_off()  power the machine off
 *
 * Both return only if the platform refused the request, so callers report
 * an I/O error after the call.
 */

#ifndef NXU_KERN_MACHINE_SYSTEM_H
#define NXU_KERN_MACHINE_SYSTEM_H

#if defined(__aarch64__)
#include <kern/arm64/system.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/system.h>
#else
#error "machine/system.h: unsupported target architecture"
#endif

#endif
