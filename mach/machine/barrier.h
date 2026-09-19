/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/machine/barrier.h
 *
 * Architecture dispatch for the memory barriers a device driver needs when it
 * shares rings and buffers with a DMA-capable device, and for the "wait a
 * moment" hint used in polling loops. Drivers call the ml_* routines and
 * never name an instruction.
 *
 *   ml_dma_wmb()   order earlier stores before later stores the device
 *                  observes (publish a descriptor, then the ring index)
 *   ml_dma_rmb()   order the load of a device-written index before the loads
 *                  of the entries it announces
 *   ml_dma_mb()    full barrier between CPU accesses and device registers
 *   ml_cpu_relax() polling-loop hint (YIELD on arm64, PAUSE on x86)
 */

#ifndef NXU_MACH_MACHINE_BARRIER_H
#define NXU_MACH_MACHINE_BARRIER_H

#if defined(__aarch64__)
#include <mach/arm64/barrier.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <mach/i386/barrier.h>
#else
#error "machine/barrier.h: unsupported target architecture"
#endif

#endif
