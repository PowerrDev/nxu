/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/machine/vm_param.h
 *
 * Architecture dispatch for the virtual-address layout, as XNU's
 * <machine/vm_param.h> does. Machine-independent VM, loader and process
 * code takes every fixed address from here instead of hard-coding a value
 * that only makes sense for one address-space width.
 *
 * Each flavour defines:
 *
 *   VMM_HIGHER_HALF_BASE   virtual address the kernel image is linked at,
 *                          added to a physical address to get its alias
 *   VM_KERN_BASE/SIZE      kernel virtual-allocation arena (vm_kern)
 *   VM_MAP_BASE/WINDOW_SIZE  per-task anonymous mmap window (vm_map)
 *   VM_SHM_BASE/WINDOW_SIZE  per-task shared-memory window (vm_shm)
 *   VM_USER_STACK_TOP      one past the last byte of the initial user stack
 *   VM_MAX_USER_ADDRESS    first address user code may not touch
 *
 * The three user windows and the initial stack must not overlap and must all
 * lie below VM_MAX_USER_ADDRESS.
 */

#ifndef NXU_KERN_MACHINE_VM_PARAM_H
#define NXU_KERN_MACHINE_VM_PARAM_H

#if defined(__aarch64__)
#include <kern/arm64/vm_param.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <kern/i386/vm_param.h>
#else
#error "machine/vm_param.h: unsupported target architecture"
#endif

#endif
