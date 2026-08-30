#ifndef NXU_KERN_BOOT_NVRAM_H
#define NXU_KERN_BOOT_NVRAM_H

#include <platform/dtb.h>

#include <stdbool.h>
#include <stdint.h>

#define NVRAM_NAME_MAX 63U
#define NVRAM_VALUE_MAX 511U
#define NVRAM_BOOT_ARGS_NAME "boot-args"

typedef enum {
	NVRAM_STATUS_OK = 0,
	NVRAM_STATUS_NOT_FOUND,
	NVRAM_STATUS_INVALID,
	NVRAM_STATUS_NO_SPACE,
	NVRAM_STATUS_READ_ONLY
} nvram_status_t;

typedef enum {
	NVRAM_BACKEND_NONE,
	NVRAM_BACKEND_DEVICE_TREE
} nvram_backend_t;

/*
 * Raw QEMU -kernel boot does not expose UEFI Runtime Services. NXU therefore
 * imports /chosen/bootargs as a read-only NVRAM view until a persistent
 * firmware-variable backend exists.
 */
bool nvram_bootstrap(const dtb_t *dtb);
nvram_status_t nvram_get(const char *name, char *value, uint32_t capacity, uint32_t *length);
nvram_status_t nvram_set(const char *name, const char *value);
nvram_backend_t nvram_backend(void);
bool nvram_is_persistent(void);
const char *nvram_status_name(nvram_status_t status);

#endif
