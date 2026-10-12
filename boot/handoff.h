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
#endif
