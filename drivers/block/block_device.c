#include <kern/console/console.h>
#include <drivers/block/block_device.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>

static block_device_t g_block_devices[BLOCK_DEVICE_MAX];
static uint32_t g_block_device_count;
static bool g_block_initialized;

/*
 * block_device_copy_name:
 *
 * Copy a bounded driver-provided name into registry-owned device storage.
 */
static void block_device_copy_name(char *destination, const char *source)
{
	uint32_t index = 0U;

	while (index < BLOCK_DEVICE_NAME_MAX && source[index] != '\0') {
		destination[index] = source[index];
		index++;
	}

	destination[index] = '\0';
}

/*
 * block_device_range_valid:
 *
 * Validate a sector range without allowing unsigned addition overflow.
 */
static bool block_device_range_valid(
	block_device_t device,
	uint64_t sector,
	uint32_t count
)
{
	if (device == 0 || !device->registered || count == 0U) return false;

	if (sector >= device->sector_count) return false;

	return (uint64_t)count <= device->sector_count - sector;
}

bool block_device_init(void)
{
	if (g_block_initialized) return true;

	for (uint32_t index = 0U; index < BLOCK_DEVICE_MAX; index++) {
		g_block_devices[index] = 0;
	}

	g_block_device_count = 0U;
	g_block_initialized = true;

	return true;
}

bool block_device_register(block_device_t device)
{
	if (!g_block_initialized || device == 0 || device->registered) return false;

	if (g_block_device_count >= BLOCK_DEVICE_MAX) return false;
	if (device->ops == 0 || device->ops->read == 0) return false;

	if (
		device->sector_count == 0ULL ||
		device->sector_size != BLOCK_SECTOR_SIZE
	) {
		return false;
	}

	if (device->logical_block_size == 0U) {
		device->logical_block_size = device->sector_size;
	}

	char generated_name[BLOCK_DEVICE_NAME_MAX + 1U] = "disk";

	generated_name[4] = (char)('0' + g_block_device_count);
	generated_name[5] = '\0';

	if (device->name[0] == '\0') {
		block_device_copy_name(device->name, generated_name);
	}

	device->device_id = g_block_device_count + 1U;
	device->registered = true;

	g_block_devices[g_block_device_count++] = device;

	return true;
}

uint32_t block_device_count(void)
{
	return g_block_device_count;
}

block_device_t block_device_get(uint32_t index)
{
	if (index >= g_block_device_count) return 0;

	return g_block_devices[index];
}

block_device_t block_device_first(void)
{
	return block_device_get(0U);
}

bool block_device_read(
	block_device_t device,
	uint64_t sector,
	uint32_t count,
	void *buffer
)
{
	if (
		buffer == 0 ||
		!block_device_range_valid(device, sector, count)
	) {
		return false;
	}

	device->read_operations++;
	bool result = device->ops->read(device, sector, count, buffer);
	if (!result) device->read_errors++;
	return result;
}

bool block_device_write(
	block_device_t device,
	uint64_t sector,
	uint32_t count,
	const void *buffer
)
{
	if (
		buffer == 0 ||
		!block_device_range_valid(device, sector, count)
	) {
		return false;
	}

	if (
		device->read_only ||
		device->ops->write == 0
	) {
		return false;
	}

	device->write_operations++;
	bool result = device->ops->write(device, sector, count, buffer);
	if (!result) device->write_errors++;
	return result;
}

bool block_device_flush(block_device_t device)
{
	if (device == 0 || !device->registered) return false;

	if (device->read_only || device->ops->flush == 0) return true;

	device->flush_operations++;
	bool result = device->ops->flush(device);
	if (!result) device->flush_errors++;
	return result;
}

bool block_device_verify(block_device_t device)
{
	if (device == 0 || !device->registered || device->sector_count == 0ULL) return false;

	uint8_t sector[BLOCK_SECTOR_SIZE];
	uint32_t samples = device->sector_count < 16ULL ? (uint32_t)device->sector_count : 16U;

	for (uint32_t index = 0U; index < samples; index++) {
		uint64_t lba = 0ULL;
		if (samples > 1U) {
			uint64_t span = device->sector_count - 1ULL;
			uint64_t divisor = samples - 1U;
			lba = (span / divisor) * index + ((span % divisor) * index) / divisor;
		}

		if (!block_device_read(device, lba, 1U, sector)) return false;
	}

	return true;
}

/*
 * block_device_dump:
 *
 * Print stable medium geometry needed while bringing up VFS and filesystems.
 */
void block_device_dump(void)
{
	kputs("VirtIOBlockFamily: devices: ");
	kputu64(g_block_device_count);
	kputc('\n');

	for (
		uint32_t index = 0U;
		index < g_block_device_count;
		index++
	) {
		block_device_t device = g_block_devices[index];

		kputs("VirtIOBlockFamily: ");
		kputs(device->name);

		kputs(", sectors: ");
		kputu64(device->sector_count);

		kputs(", sector size: ");
		kputu64(device->sector_size);

		kputs(", logical block size: ");
		kputu64(device->logical_block_size);

		kputs(", read-only: ");
		kputln(
			device->read_only
				? "yes"
				: "no"
		);
	}
}
