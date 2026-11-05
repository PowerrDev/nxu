/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/handoff.h
 *
 * Shared boot handoff layouts for NXU loaders and kernel architectures.
 */
#ifndef NXU_BOOT_HANDOFF_H
#define NXU_BOOT_HANDOFF_H

#include <stdint.h>

/* NXU extension after the standard 116-byte Multiboot 1 information. */
#define NXU_BOOT_FIRMWARE_FLAG (1U << 31)
#define NXU_BOOT_FIRMWARE_OFFSET 116U
#define NXU_BOOT_FIRMWARE_MAGIC 0x4E584246U

struct __attribute__((packed)) nxu_boot_firmware {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint64_t rsdp;
};

/*
 * One converted UEFI memory-map entry passed to the x86-64 kernel. Explicit
 * reserved fields keep the wire layout stable when a compiler packs structs.
 */
struct nxu_boot_memory_map_entry {
	uint32_t size;
	uint32_t reserved0;
	uint64_t address;
	uint64_t length;
	uint32_t type;
	uint32_t reserved1;
};

_Static_assert(sizeof(struct nxu_boot_memory_map_entry) == 32U, "NXU boot memory-map entry layout");

#define NXU_BOOT_X86_64_MAGIC 0x4E585536U
#define NXU_BOOT_X86_64_VERSION 1U

/* Passed in RDI by NxuBoot after ExitBootServices and before kernel startup. */
struct __attribute__((packed)) nxu_boot_x86_64 {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t reserved;
	uint64_t rsdp;
	uint64_t memory_map;
	uint64_t memory_map_size;
	uint64_t memory_descriptor_size;
	uint64_t bootargs;
	uint64_t stack_top;
};

#endif
