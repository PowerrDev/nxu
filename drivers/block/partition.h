#ifndef NXU_DRIVERS_BLOCK_PARTITION_H
#define NXU_DRIVERS_BLOCK_PARTITION_H

#include <drivers/block/block_device.h>

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_PARTITION_NAME_MAX 35U

typedef enum {
	BLOCK_LAYOUT_RAW = 0,
	BLOCK_LAYOUT_MBR = 1,
	BLOCK_LAYOUT_GPT = 2
} block_layout_scheme_t;

typedef enum {
	BLOCK_PARTITION_STATUS_OK = 0,
	BLOCK_PARTITION_STATUS_INVALID,
	BLOCK_PARTITION_STATUS_READ_ONLY,
	BLOCK_PARTITION_STATUS_IO_ERROR,
	BLOCK_PARTITION_STATUS_NO_SPACE,
	BLOCK_PARTITION_STATUS_NOT_SUPPORTED
} block_partition_status_t;

/*
 * Role a new partition is created with.
 *
 * Kept as a role rather than a raw GPT type GUID so callers (the syscall layer
 * and, through it, Disk Utility) never carry a GUID table; the mapping to a
 * GUID, or to an MBR type byte on a legacy map, lives here.
 */
typedef enum {
	BLOCK_PARTITION_ROLE_DATA = 0,
	BLOCK_PARTITION_ROLE_SYSTEM = 1,
	BLOCK_PARTITION_ROLE_EFI = 2
} block_partition_role_t;

typedef struct {
	block_layout_scheme_t scheme;
	uint32_t partition_count;
} block_layout_info_t;

typedef struct {
	block_layout_scheme_t scheme;
	uint32_t index;
	uint64_t start_sector;
	uint64_t sector_count;
	uint32_t type;
	char name[BLOCK_PARTITION_NAME_MAX + 1U];
} block_partition_info_t;

bool block_layout_inspect(block_device_t device, block_layout_info_t *info);
bool block_partition_get(block_device_t device, uint32_t index, block_partition_info_t *info);
block_partition_status_t block_layout_initialize_gpt(block_device_t device);
block_partition_status_t block_partition_create(
	block_device_t device,
	uint64_t sector_count,
	const char *name,
	block_partition_role_t role,
	block_partition_info_t *info
);
block_partition_status_t block_partition_delete(block_device_t device, uint32_t index);

#endif
