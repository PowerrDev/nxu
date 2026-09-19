/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/multiboot.h
 *
 * The parts of the Multiboot 1 boot-information structure the early kernel
 * reads. QEMU's -kernel loader and GRUB both hand the kernel a pointer to it
 * in %ebx, with MULTIBOOT_BOOTLOADER_MAGIC in %eax.
 */

#ifndef NXU_MACH_I386_MULTIBOOT_H
#define NXU_MACH_I386_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002U

#define MULTIBOOT_INFO_MEMORY (1U << 0U)
#define MULTIBOOT_INFO_CMDLINE (1U << 2U)
#define MULTIBOOT_INFO_MMAP (1U << 6U)

typedef struct {
	uint32_t flags;
	uint32_t mem_lower; /* KiB below 1 MiB */
	uint32_t mem_upper; /* KiB above 1 MiB */
	uint32_t boot_device;
	uint32_t cmdline; /* physical address of a C string */
	uint32_t mods_count;
	uint32_t mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length;
	uint32_t mmap_addr;
} multiboot_info_t;

#endif
