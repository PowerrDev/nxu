#ifndef NXU_DRIVERS_BLOCK_BLOCK_DEVICE_H
#define NXU_DRIVERS_BLOCK_BLOCK_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_DEVICE_MAX 8U
#define BLOCK_DEVICE_NAME_MAX 31U
#define BLOCK_SECTOR_SIZE 512U

typedef struct block_device *block_device_t;

typedef struct {
	bool (*read)(
		block_device_t device,
		uint64_t sector,
		uint32_t count,
		void *buffer
	);

	bool (*write)(
		block_device_t device,
		uint64_t sector,
		uint32_t count,
		const void *buffer
	);

	bool (*flush)(block_device_t device);
} block_device_ops_t;

/*
 * struct block_device
 *
 * Transport-independent description of one sector-addressed block device.
 *
 * sector_count and sector_size describe the protocol-visible medium. The
 * block layer validates all ranges before entering the driver. driver_data
 * is owned by the class driver and remains valid while the device is
 * registered.
 *
 * The initial NXU block layer is intentionally small: registration is
 * static, devices are never hot-unplugged, and I/O is synchronous. VFS and
 * filesystem code must use this interface rather than reaching into VirtIO.
 */
struct block_device {
	uint32_t device_id;
	char name[BLOCK_DEVICE_NAME_MAX + 1U];

	uint64_t sector_count;
	uint32_t sector_size;
	uint32_t logical_block_size;

	bool read_only;
	bool flush_supported;
	bool registered;

	uint64_t read_operations;
	uint64_t write_operations;
	uint64_t flush_operations;
	uint64_t read_errors;
	uint64_t write_errors;
	uint64_t flush_errors;

	const block_device_ops_t *ops;
	void *driver_data;
};

/*
 * block_device_init:
 *
 * Initialize the global block-device registry. Repeated initialization is
 * harmless.
 */
bool block_device_init(void);

/*
 * block_device_register:
 *
 * Publish a fully initialized device to upper layers. The caller retains
 * ownership of the structure and driver_data for the lifetime of the
 * registration.
 */
bool block_device_register(block_device_t device);

uint32_t block_device_count(void);
block_device_t block_device_get(uint32_t index);
block_device_t block_device_first(void);

/*
 * block_device_read:
 *
 * Read count protocol sectors beginning at sector into caller storage.
 * Requests outside the medium are rejected before entering the driver.
 */
bool block_device_read(
	block_device_t device,
	uint64_t sector,
	uint32_t count,
	void *buffer
);

/*
 * block_device_write:
 *
 * Write count protocol sectors beginning at sector. Read-only devices are
 * rejected by the block layer before driver dispatch.
 */
bool block_device_write(
	block_device_t device,
	uint64_t sector,
	uint32_t count,
	const void *buffer
);

/*
 * block_device_flush:
 *
 * Commit volatile device write caches when the driver exposes flush support.
 * A device without a flush operation is treated as already synchronous.
 */
bool block_device_flush(block_device_t device);
bool block_device_verify(block_device_t device);

void block_device_dump(void);

#endif
