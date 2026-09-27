/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/vm_param.h
 *
 * arm64 address-space layout: 39-bit virtual addresses, kernel in the upper
 * (TTBR1) half, user in the lower (TTBR0) half. See
 * <kern/machine/vm_param.h> for what each name means.
 */

#ifndef NXU_KERN_ARM64_VM_PARAM_H
#define NXU_KERN_ARM64_VM_PARAM_H

#define VMM_HIGHER_HALF_BASE 0xFFFFFF8000000000ULL

#define VM_KERN_BASE 0xFFFFFFE000000000UL
/*
 * 512 MiB of kernel virtual space for vm_kern (backed on demand): a 2x
 * desktop keeps several full-screen buffers (~17 MiB each at 2732x1536) and
 * a window's backing store per app, which 64 MiB could not hold.
 */
#define VM_KERN_SIZE 0x0000000020000000ULL

#define VM_MAP_BASE 0x0000005000000000ULL
#define VM_MAP_WINDOW_SIZE 0x0000000100000000ULL

#define VM_SHM_BASE 0x0000006000000000ULL
#define VM_SHM_WINDOW_SIZE 0x0000000040000000ULL

/*
 * The arm64 port resolves user page faults (vm/vm_fault.h), so anonymous
 * regions may be populated lazily on first touch.
 */
#define VM_DEMAND_PAGING 1

#define VM_USER_STACK_TOP 0x0000007FFFFFF000ULL
#define VM_MAX_USER_ADDRESS 0x0000008000000000ULL

#endif
