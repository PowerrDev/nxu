/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/gdt.h
 *
 * Long-mode GDT and TSS setup used by the x86-64 trap entry path.
 */
#ifndef NXU_KERN_X86_64_GDT_H
#define NXU_KERN_X86_64_GDT_H

#include <stdbool.h>
#include <stdint.h>

#define X86_KERNEL_CODE_SELECTOR 0x08U
#define X86_KERNEL_DATA_SELECTOR 0x10U
#define X86_USER_DATA_SELECTOR 0x1BU
#define X86_USER_CODE_SELECTOR 0x23U
#define X86_TSS_SELECTOR 0x28U

bool x86_gdt_initialize(uint64_t kernel_stack_top);

#endif
