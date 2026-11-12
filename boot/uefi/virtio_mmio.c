/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/virtio_mmio.c
 *
 * The VirtIO Sound device over VirtIO-MMIO, for NBL on AArch64 firmware: the
 * Device Tree lists the register frames, the firmware maps them one to one.
 * Only the modern (version 2) register layout is driven.
 */

#include "fdt.h"
#include "nbl.h"

#define MMIO_MAGIC 0x000U
#define MMIO_VERSION 0x004U
#define MMIO_DEVICE_ID 0x008U
#define MMIO_DEVICE_FEATURES 0x010U
#define MMIO_DEVICE_FEATURES_SELECT 0x014U
#define MMIO_DRIVER_FEATURES 0x020U
#define MMIO_DRIVER_FEATURES_SELECT 0x024U
#define MMIO_QUEUE_SELECT 0x030U
#define MMIO_QUEUE_SIZE_MAX 0x034U
#define MMIO_QUEUE_SIZE 0x038U
#define MMIO_QUEUE_READY 0x044U
#define MMIO_QUEUE_NOTIFY 0x050U
#define MMIO_STATUS 0x070U
#define MMIO_QUEUE_DESC 0x080U
#define MMIO_QUEUE_DRIVER 0x090U
#define MMIO_QUEUE_DEVICE 0x0A0U
#define MMIO_CONFIG 0x100U

#define MMIO_MAGIC_VALUE 0x74726976U
#define VIRTIO_DEVICE_SOUND 25U

static volatile uint32_t *
reg(nbl_virtio *device, uint32_t offset)
{
	return (volatile uint32_t *)(device->common + offset);
}

static void
write64(nbl_virtio *device, uint32_t offset, uint64_t value)
{
	*reg(device, offset) = (uint32_t)value;
	*reg(device, offset + 4U) = (uint32_t)(value >> 32);
}

bool
nbl_virtio_find_sound(nbl_virtio *device)
{
	const void *fdt = nbl_fdt_firmware();
	uint64_t base;

	nbl_zero(device, sizeof(*device));
	if (!fdt) {
		return false;
	}

	for (uint32_t index = 0; nbl_fdt_compatible_reg(fdt, "virtio,mmio", index, &base); index++) {
		device->common = (volatile uint8_t *)(uintptr_t)base;
		if (*reg(device, MMIO_MAGIC) == MMIO_MAGIC_VALUE && *reg(device, MMIO_VERSION) == 2U &&
			*reg(device, MMIO_DEVICE_ID) == VIRTIO_DEVICE_SOUND) {
			device->config = device->common + MMIO_CONFIG;
			return true;
		}
	}

	nbl_zero(device, sizeof(*device));
	return false;
}

uint8_t
nbl_virtio_status(nbl_virtio *device)
{
	return (uint8_t)*reg(device, MMIO_STATUS);
}

void
nbl_virtio_set_status(nbl_virtio *device, uint8_t value)
{
	*reg(device, MMIO_STATUS) = value;
}

uint64_t
nbl_virtio_device_features(nbl_virtio *device)
{
	*reg(device, MMIO_DEVICE_FEATURES_SELECT) = 1;

	uint64_t features = (uint64_t)*reg(device, MMIO_DEVICE_FEATURES) << 32;

	*reg(device, MMIO_DEVICE_FEATURES_SELECT) = 0;
	return features | *reg(device, MMIO_DEVICE_FEATURES);
}

void
nbl_virtio_set_driver_features(nbl_virtio *device, uint64_t features)
{
	*reg(device, MMIO_DRIVER_FEATURES_SELECT) = 0;
	*reg(device, MMIO_DRIVER_FEATURES) = (uint32_t)features;
	*reg(device, MMIO_DRIVER_FEATURES_SELECT) = 1;
	*reg(device, MMIO_DRIVER_FEATURES) = (uint32_t)(features >> 32);
}

uint16_t
nbl_virtio_queue_max(nbl_virtio *device, uint16_t queue)
{
	*reg(device, MMIO_QUEUE_SELECT) = queue;

	uint32_t size = *reg(device, MMIO_QUEUE_SIZE_MAX);

	return size > 0xFFFFU ? 0xFFFFU : (uint16_t)size;
}

void
nbl_virtio_queue_enable(nbl_virtio *device, uint16_t queue, uint16_t size, uint64_t descriptors, uint64_t driver,
						uint64_t device_area)
{
	*reg(device, MMIO_QUEUE_SELECT) = queue;
	*reg(device, MMIO_QUEUE_SIZE) = size;
	write64(device, MMIO_QUEUE_DESC, descriptors);
	write64(device, MMIO_QUEUE_DRIVER, driver);
	write64(device, MMIO_QUEUE_DEVICE, device_area);
	*reg(device, MMIO_QUEUE_READY) = 1;
}

void
nbl_virtio_queue_notify(nbl_virtio *device, uint16_t queue)
{
	*reg(device, MMIO_QUEUE_NOTIFY) = queue;
}

uint32_t
nbl_virtio_config32(nbl_virtio *device, uint32_t offset)
{
	return *(volatile uint32_t *)(device->config + offset);
}
