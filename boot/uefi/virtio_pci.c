/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/virtio_pci.c
 *
 * The VirtIO Sound device over PCI, for NBL on x64 firmware: find the modern
 * device in configuration space and move its common-configuration registers.
 * The firmware has already assigned the BARs and maps them one to one.
 */

#include "nbl.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* Vendor 0x1AF4, modern device ID 0x1040 + 25 (sound). */
#define VIRTIO_SOUND_PCI_ID 0x10591AF4U

#define PCI_COMMAND 0x04U
#define PCI_COMMAND_MEMORY 0x02U
#define PCI_COMMAND_BUS_MASTER 0x04U
#define PCI_STATUS_CAPABILITIES 0x10U
#define PCI_HEADER_TYPE 0x0EU
#define PCI_CAPABILITY_POINTER 0x34U
#define PCI_CAPABILITY_VENDOR 0x09U

#define VIRTIO_PCI_CAP_COMMON 1U
#define VIRTIO_PCI_CAP_NOTIFY 2U
#define VIRTIO_PCI_CAP_DEVICE 4U

/* The common configuration structure of VirtIO 1.x, section 4.1.4.3. */
#define COMMON_DEVICE_FEATURE_SELECT 0U
#define COMMON_DEVICE_FEATURE 4U
#define COMMON_DRIVER_FEATURE_SELECT 8U
#define COMMON_DRIVER_FEATURE 12U
#define COMMON_STATUS 20U
#define COMMON_QUEUE_SELECT 22U
#define COMMON_QUEUE_SIZE 24U
#define COMMON_QUEUE_MSIX 26U
#define COMMON_QUEUE_ENABLE 28U
#define COMMON_QUEUE_NOTIFY_OFF 30U
#define COMMON_QUEUE_DESC 32U
#define COMMON_QUEUE_DRIVER 40U
#define COMMON_QUEUE_DEVICE 48U

static void
out32(uint16_t port, uint32_t value)
{
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t
in32(uint16_t port)
{
	uint32_t value;

	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static uint32_t
pci_address(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	return 0x80000000U | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFCU);
}

static uint32_t
pci_read32(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	out32(PCI_CONFIG_ADDRESS, pci_address(bus, slot, function, offset));
	return in32(PCI_CONFIG_DATA);
}

static void
pci_write32(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint32_t value)
{
	out32(PCI_CONFIG_ADDRESS, pci_address(bus, slot, function, offset));
	out32(PCI_CONFIG_DATA, value);
}

static uint8_t
pci_read8(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	return (uint8_t)(pci_read32(bus, slot, function, offset) >> ((offset & 3U) * 8U));
}

/*
 * Routine:     bar_address
 * Purpose:
 *              The memory address the firmware gave one BAR; 0 for an I/O
 *              BAR or one that was never assigned.
 */
static uint64_t
bar_address(uint32_t bus, uint32_t slot, uint32_t function, uint32_t bar)
{
	if (bar > 5) {
		return 0;
	}

	uint32_t low = pci_read32(bus, slot, function, 0x10U + bar * 4U);

	if (low & 1U) {
		return 0;
	}

	uint64_t address = low & ~0xFULL;

	if (((low >> 1) & 3U) == 2U && bar < 5) {
		address |= (uint64_t)pci_read32(bus, slot, function, 0x14U + bar * 4U) << 32;
	}

	return address;
}

/*
 * Routine:     map_capabilities
 * Purpose:
 *              Walk the vendor capabilities for the common, notification and
 *              device-specific windows of a modern VirtIO device.
 */
static bool
map_capabilities(uint32_t bus, uint32_t slot, uint32_t function, nbl_virtio *device)
{
	if (!((pci_read32(bus, slot, function, PCI_COMMAND) >> 16) & PCI_STATUS_CAPABILITIES)) {
		return false;
	}

	uint32_t pointer = pci_read8(bus, slot, function, PCI_CAPABILITY_POINTER) & 0xFCU;

	for (unsigned guard = 0; pointer >= 0x40U && guard < 48; guard++) {
		uint32_t header = pci_read32(bus, slot, function, pointer);

		if ((header & 0xFFU) == PCI_CAPABILITY_VENDOR) {
			uint32_t type = header >> 24;
			uint32_t bar = pci_read8(bus, slot, function, pointer + 4U);
			uint64_t base = bar_address(bus, slot, function, bar);
			uint32_t offset = pci_read32(bus, slot, function, pointer + 8U);

			if (base) {
				volatile uint8_t *window = (volatile uint8_t *)(uintptr_t)(base + offset);

				if (type == VIRTIO_PCI_CAP_COMMON && !device->common) {
					device->common = window;
				} else if (type == VIRTIO_PCI_CAP_NOTIFY && !device->notify) {
					device->notify = window;
					device->notify_multiplier = pci_read32(bus, slot, function, pointer + 16U);
				} else if (type == VIRTIO_PCI_CAP_DEVICE && !device->config) {
					device->config = window;
				}
			}
		}

		pointer = (header >> 8) & 0xFCU;
	}

	return device->common && device->notify && device->config;
}

bool
nbl_virtio_find_sound(nbl_virtio *device)
{
	nbl_zero(device, sizeof(*device));

	for (uint32_t bus = 0; bus < 256; bus++) {
		for (uint32_t slot = 0; slot < 32; slot++) {
			if ((pci_read32(bus, slot, 0, 0) & 0xFFFFU) == 0xFFFFU) {
				continue;
			}

			uint32_t functions = pci_read8(bus, slot, 0, PCI_HEADER_TYPE) & 0x80U ? 8 : 1;

			for (uint32_t function = 0; function < functions; function++) {
				if (pci_read32(bus, slot, function, 0) != VIRTIO_SOUND_PCI_ID) {
					continue;
				}

				uint32_t command = pci_read32(bus, slot, function, PCI_COMMAND);

				/* The upper half is the status register: its bits clear when written as one. */
				pci_write32(bus, slot, function, PCI_COMMAND,
							(command & 0xFFFFU) | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
				if (map_capabilities(bus, slot, function, device)) {
					return true;
				}

				nbl_zero(device, sizeof(*device));
			}
		}
	}

	return false;
}

static volatile uint8_t *
common8(nbl_virtio *device, uint32_t offset)
{
	return device->common + offset;
}

static volatile uint16_t *
common16(nbl_virtio *device, uint32_t offset)
{
	return (volatile uint16_t *)(device->common + offset);
}

static volatile uint32_t *
common32(nbl_virtio *device, uint32_t offset)
{
	return (volatile uint32_t *)(device->common + offset);
}

static void
common_write64(nbl_virtio *device, uint32_t offset, uint64_t value)
{
	*common32(device, offset) = (uint32_t)value;
	*common32(device, offset + 4U) = (uint32_t)(value >> 32);
}

uint8_t
nbl_virtio_status(nbl_virtio *device)
{
	return *common8(device, COMMON_STATUS);
}

void
nbl_virtio_set_status(nbl_virtio *device, uint8_t value)
{
	*common8(device, COMMON_STATUS) = value;
}

uint64_t
nbl_virtio_device_features(nbl_virtio *device)
{
	*common32(device, COMMON_DEVICE_FEATURE_SELECT) = 1;

	uint64_t features = (uint64_t)*common32(device, COMMON_DEVICE_FEATURE) << 32;

	*common32(device, COMMON_DEVICE_FEATURE_SELECT) = 0;
	return features | *common32(device, COMMON_DEVICE_FEATURE);
}

void
nbl_virtio_set_driver_features(nbl_virtio *device, uint64_t features)
{
	*common32(device, COMMON_DRIVER_FEATURE_SELECT) = 0;
	*common32(device, COMMON_DRIVER_FEATURE) = (uint32_t)features;
	*common32(device, COMMON_DRIVER_FEATURE_SELECT) = 1;
	*common32(device, COMMON_DRIVER_FEATURE) = (uint32_t)(features >> 32);
}

uint16_t
nbl_virtio_queue_max(nbl_virtio *device, uint16_t queue)
{
	*common16(device, COMMON_QUEUE_SELECT) = queue;
	return *common16(device, COMMON_QUEUE_SIZE);
}

void
nbl_virtio_queue_enable(nbl_virtio *device, uint16_t queue, uint16_t size, uint64_t descriptors, uint64_t driver,
						uint64_t device_area)
{
	*common16(device, COMMON_QUEUE_SELECT) = queue;
	*common16(device, COMMON_QUEUE_SIZE) = size;
	/* No MSI-X vector: the loader polls and asks the device not to interrupt at all. */
	*common16(device, COMMON_QUEUE_MSIX) = 0xFFFFU;
	common_write64(device, COMMON_QUEUE_DESC, descriptors);
	common_write64(device, COMMON_QUEUE_DRIVER, driver);
	common_write64(device, COMMON_QUEUE_DEVICE, device_area);
	*common16(device, COMMON_QUEUE_ENABLE) = 1;
}

void
nbl_virtio_queue_notify(nbl_virtio *device, uint16_t queue)
{
	*common16(device, COMMON_QUEUE_SELECT) = queue;

	uint32_t offset = *common16(device, COMMON_QUEUE_NOTIFY_OFF) * device->notify_multiplier;

	*(volatile uint16_t *)(device->notify + offset) = queue;
}

uint32_t
nbl_virtio_config32(nbl_virtio *device, uint32_t offset)
{
	return *(volatile uint32_t *)(device->config + offset);
}
