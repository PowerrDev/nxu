/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/vm_param.h
 *
 * x86-64 virtual-address layout for the initial low-memory kernel image,
 * higher-half direct map, kernel arena, and lower-half user mappings.
 */
#ifndef NXU_KERN_X86_64_VM_PARAM_H
#define NXU_KERN_X86_64_VM_PARAM_H

#define VMM_HIGHER_HALF_BASE 0xFFFF800000000000ULL
#define VM_DIRECT_MAP_SIZE 0x40000000ULL

#define VM_KERN_BASE 0xFFFF900000000000ULL
#define VM_KERN_SIZE 0x0000000020000000ULL

#define VM_MMIO_BASE 0xFFFFA00000000000ULL
#define VM_MMIO_SIZE 0x0000000010000000ULL

#define VM_MAP_BASE 0x0000400000000000ULL
#define VM_MAP_WINDOW_SIZE 0x0000000200000000ULL

#define VM_SHM_BASE 0x0000600000000000ULL
#define VM_SHM_WINDOW_SIZE 0x0000000100000000ULL

#define VM_DEMAND_PAGING 0

#define VM_USER_STACK_TOP 0x00007FFFFFFFE000ULL
#define VM_MAX_USER_ADDRESS 0x0000800000000000ULL

#endif
