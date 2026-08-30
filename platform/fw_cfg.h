#ifndef NXU_PLATFORM_FW_CFG_H
#define NXU_PLATFORM_FW_CFG_H

#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint16_t selector;
	uint32_t size;
} fw_cfg_file_t;

bool fw_cfg_find_file(
	const platform_region_t *region,
	const char *name,
	fw_cfg_file_t *file
);

bool fw_cfg_dma_write(
	const platform_region_t *region,
	uint16_t selector,
	const void *data,
	uint32_t size
);

#endif
