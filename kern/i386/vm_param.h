/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/vm_param.h
 *
 * i386 address-space layout: 32-bit virtual addresses split 3 GiB user /
 * 1 GiB kernel, in the classic higher-half arrangement. See
 * <kern/machine/vm_param.h> for what each name means.
 *
 *   0x00000000 - 0x00000FFF   unmapped null guard
 *   0x00001000 - 0x3FFFFFFF   user image (ELF segments)
 *   0x40000000 - 0x5FFFFFFF   VM_MAP window   (anonymous mmap)
 *   0x60000000 - 0x6FFFFFFF   VM_SHM window   (shared memory)
 *   0x70000000 - 0xBFFFFFFF   free for user growth
 *   0xBFFFF000                VM_USER_STACK_TOP (stack grows down)
 *   0xC0000000 - 0xE7FFFFFF   kernel direct map of physical 0 - 640 MiB
 *   0xE8000000 - 0xF7FFFFFF   VM_KERN arena (vm_kern_allocate)
 *   0xF8000000 - 0xFFFFFFFF   device (MMIO) window
 *
 * Only physical memory below VM_DIRECT_MAP_SIZE is directly mapped and
 * usable; RAM above it is ignored (there is no highmem support).
 */

#ifndef NXU_KERN_I386_VM_PARAM_H
#define NXU_KERN_I386_VM_PARAM_H

#define VMM_HIGHER_HALF_BASE 0xC0000000UL

/*
 * 640 MiB, not the 768 it was: the desktop's buffers (each app window's at
 * its largest size, a few times over, at 2x) filled a 128 MiB arena with two
 * apps open and a resize, while the VMs this boots in have 512 MiB of RAM.
 * The 128 MiB taken from the direct map's unused top doubles the arena.
 */
#define VM_DIRECT_MAP_SIZE 0x28000000UL

#define VM_KERN_BASE 0xE8000000UL
/* 256 MiB, up to the MMIO window. */
#define VM_KERN_SIZE 0x10000000ULL

#define VM_MMIO_BASE 0xF8000000UL
#define VM_MMIO_SIZE 0x08000000UL

#define VM_MAP_BASE 0x40000000ULL
#define VM_MAP_WINDOW_SIZE 0x20000000ULL

#define VM_SHM_BASE 0x60000000ULL
#define VM_SHM_WINDOW_SIZE 0x10000000ULL

/*
 * The i386 port has no user page-fault resolver yet, so anonymous regions
 * are populated eagerly and fork is unavailable.
 */
#define VM_DEMAND_PAGING 0

#define VM_USER_STACK_TOP 0xBFFFF000ULL
#define VM_MAX_USER_ADDRESS 0xC0000000ULL

#endif
