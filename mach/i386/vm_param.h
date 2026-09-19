/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/vm_param.h
 *
 * i386 address-space layout: 32-bit virtual addresses split 3 GiB user /
 * 1 GiB kernel, in the classic higher-half arrangement. See
 * <mach/machine/vm_param.h> for what each name means.
 *
 *   0x00000000 - 0x00000FFF   unmapped null guard
 *   0x00001000 - 0x3FFFFFFF   user image (ELF segments)
 *   0x40000000 - 0x5FFFFFFF   VM_MAP window   (anonymous mmap)
 *   0x60000000 - 0x6FFFFFFF   VM_SHM window   (shared memory)
 *   0x70000000 - 0xBFFFFFFF   free for user growth
 *   0xBFFFF000                VM_USER_STACK_TOP (stack grows down)
 *   0xC0000000 - 0xEFFFFFFF   kernel direct map of physical 0 - 768 MiB
 *   0xF0000000 - 0xF3FFFFFF   VM_KERN arena (vm_kern_allocate)
 *   0xF4000000 - 0xF7FFFFFF   reserved (fixmap / temporary mappings)
 *   0xF8000000 - 0xFFFFFFFF   device (MMIO) window
 *
 * Only physical memory below VM_DIRECT_MAP_SIZE is directly mapped and
 * usable; RAM above it is ignored (there is no highmem support).
 */

#ifndef NXU_MACH_I386_VM_PARAM_H
#define NXU_MACH_I386_VM_PARAM_H

#define VMM_HIGHER_HALF_BASE 0xC0000000UL

#define VM_DIRECT_MAP_SIZE 0x30000000UL

#define VM_KERN_BASE 0xF0000000UL
#define VM_KERN_SIZE 0x04000000ULL

#define VM_MMIO_BASE 0xF8000000UL
#define VM_MMIO_SIZE 0x08000000UL

#define VM_MAP_BASE 0x40000000ULL
#define VM_MAP_WINDOW_SIZE 0x20000000ULL

#define VM_SHM_BASE 0x60000000ULL
#define VM_SHM_WINDOW_SIZE 0x10000000ULL

#define VM_USER_STACK_TOP 0xBFFFF000ULL
#define VM_MAX_USER_ADDRESS 0xC0000000ULL

#endif
