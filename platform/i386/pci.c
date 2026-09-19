/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/pci.c
 *
 * PCI configuration mechanism 1 and bus enumeration. See pci.h.
 */

#include <platform/i386/pci.h>
#include <platform/i386/portio.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

#define PCI_CONFIG_ADDRESS_PORT 0xCF8U
#define PCI_CONFIG_DATA_PORT 0xCFCU
#define PCI_CONFIG_ENABLE 0x80000000U

#define PCI_MAX_SCAN_DEPTH 8U

#define PCI_HEADER_TYPE_MASK 0x7FU
#define PCI_HEADER_MULTIFUNCTION 0x80U
#define PCI_HEADER_TYPE_NORMAL 0x00U
#define PCI_HEADER_TYPE_BRIDGE 0x01U

#define PCI_BAR_IO 0x01U
#define PCI_BAR_TYPE_MASK 0x06U
#define PCI_BAR_TYPE_64 0x04U
#define PCI_BAR_PREFETCHABLE 0x08U

/*
 * Extension hook: the VM area supplies this to map device memory once paging
 * is on. Weak so the devices area builds and runs without it.
 */
extern void *i386_mmio_map(uint64_t physical, uint64_t size) __attribute__((weak));

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static uint32_t g_pci_device_count;
static uint32_t g_pci_bus_count;
static bool g_pci_present;
static bool g_pci_initialized;

static void pci_select(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
	outl(
		PCI_CONFIG_ADDRESS_PORT,
		PCI_CONFIG_ENABLE |
			((uint32_t)bus << 16U) |
			((uint32_t)(slot & 0x1FU) << 11U) |
			((uint32_t)(function & 0x07U) << 8U) |
			((uint32_t)offset & 0xFCU)
	);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
	pci_select(bus, slot, function, offset);
	return inl(PCI_CONFIG_DATA_PORT);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
	pci_select(bus, slot, function, offset);
	return inw((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 2U)));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
	pci_select(bus, slot, function, offset);
	return inb((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 3U)));
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value)
{
	pci_select(bus, slot, function, offset);
	outl(PCI_CONFIG_DATA_PORT, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value)
{
	pci_select(bus, slot, function, offset);
	outw((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 2U)), value);
}

void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint8_t value)
{
	pci_select(bus, slot, function, offset);
	outb((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 3U)), value);
}

/*
 * pci_detect:
 *
 * Configuration mechanism 1 is present when the address register latches what
 * was written to it. Restores the register afterwards.
 */
static bool pci_detect(void)
{
	uint32_t saved = inl(PCI_CONFIG_ADDRESS_PORT);

	outl(PCI_CONFIG_ADDRESS_PORT, PCI_CONFIG_ENABLE);

	bool present = inl(PCI_CONFIG_ADDRESS_PORT) == PCI_CONFIG_ENABLE;

	if (present) {
		/* And a host bridge has to answer at 00:00.0. */
		present = pci_config_read16(0U, 0U, 0U, PCI_CONFIG_VENDOR_ID) != PCI_VENDOR_INVALID;
	}

	outl(PCI_CONFIG_ADDRESS_PORT, saved);
	return present;
}

/*
 * pci_size_bars:
 *
 * Size and record the BARs of one function. Decoding is switched off while a
 * BAR is probed with all ones, then the original value (the address firmware
 * assigned) and the command register are restored. A 64-bit BAR consumes the
 * following register as its upper half.
 */
static void pci_size_bars(pci_device_t *device)
{
	uint32_t count = 0U;

	if (device->header_type == PCI_HEADER_TYPE_NORMAL) count = PCI_MAX_BARS;
	else if (device->header_type == PCI_HEADER_TYPE_BRIDGE) count = 2U;

	uint8_t bus = device->bus;
	uint8_t slot = device->slot;
	uint8_t function = device->function;
	uint16_t command = pci_config_read16(bus, slot, function, PCI_CONFIG_COMMAND);

	pci_config_write16(bus, slot, function, PCI_CONFIG_COMMAND, (uint16_t)(command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));

	for (uint32_t index = 0U; index < count; index++) {
		uint8_t offset = (uint8_t)(PCI_CONFIG_BAR0 + index * 4U);
		uint32_t original = pci_config_read32(bus, slot, function, offset);

		pci_config_write32(bus, slot, function, offset, 0xFFFFFFFFU);
		uint32_t probe = pci_config_read32(bus, slot, function, offset);
		pci_config_write32(bus, slot, function, offset, original);

		if (probe == 0U || probe == 0xFFFFFFFFU) continue;

		pci_bar_t *bar = &device->bars[index];

		if ((original & PCI_BAR_IO) != 0U) {
			uint32_t mask = probe & ~0x3U;

			bar->io = true;
			bar->base = original & ~0x3U;
			bar->size = (uint64_t)((~mask + 1U) & 0xFFFFU);
			bar->valid = bar->size != 0ULL;
			continue;
		}

		bar->io = false;
		bar->prefetchable = (original & PCI_BAR_PREFETCHABLE) != 0U;
		bar->is64 = (original & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
		bar->base = original & ~0xFU;

		uint64_t size_mask = probe & ~0xFU;

		if (bar->is64 && index + 1U < count) {
			uint8_t high_offset = (uint8_t)(offset + 4U);
			uint32_t high_original = pci_config_read32(bus, slot, function, high_offset);

			pci_config_write32(bus, slot, function, high_offset, 0xFFFFFFFFU);
			uint32_t high_probe = pci_config_read32(bus, slot, function, high_offset);
			pci_config_write32(bus, slot, function, high_offset, high_original);

			bar->base |= (uint64_t)high_original << 32U;
			size_mask |= (uint64_t)high_probe << 32U;
			bar->size = ~size_mask + 1ULL;

			/* The upper half belongs to this BAR, not a BAR of its own. */
			index++;
		} else {
			bar->size = (uint64_t)((~(uint32_t)size_mask) + 1U);
		}

		bar->valid = bar->size != 0ULL;
	}

	pci_config_write16(bus, slot, function, PCI_CONFIG_COMMAND, command);
}

static void pci_scan_bus(uint8_t bus, uint32_t depth);

static void pci_probe_function(uint8_t bus, uint8_t slot, uint8_t function, uint32_t depth)
{
	if (g_pci_device_count >= PCI_MAX_DEVICES) return;

	uint16_t vendor = pci_config_read16(bus, slot, function, PCI_CONFIG_VENDOR_ID);

	if (vendor == PCI_VENDOR_INVALID) return;

	pci_device_t *device = &g_pci_devices[g_pci_device_count];
	uint32_t class_word = pci_config_read32(bus, slot, function, PCI_CONFIG_REVISION);

	*device = (pci_device_t){ 0 };
	device->bus = bus;
	device->slot = slot;
	device->function = function;
	device->vendor_id = vendor;
	device->device_id = pci_config_read16(bus, slot, function, PCI_CONFIG_DEVICE_ID);
	device->revision = (uint8_t)class_word;
	device->prog_if = (uint8_t)(class_word >> 8U);
	device->subclass = (uint8_t)(class_word >> 16U);
	device->class_code = (uint8_t)(class_word >> 24U);
	device->header_type = (uint8_t)(pci_config_read8(bus, slot, function, PCI_CONFIG_HEADER_TYPE) & PCI_HEADER_TYPE_MASK);

	if (device->header_type == PCI_HEADER_TYPE_NORMAL) {
		device->subsystem_vendor_id = pci_config_read16(bus, slot, function, PCI_CONFIG_SUBSYSTEM_VENDOR);
		device->subsystem_id = pci_config_read16(bus, slot, function, PCI_CONFIG_SUBSYSTEM_ID);
	}

	if (device->header_type != 2U) {
		device->interrupt_line = pci_config_read8(bus, slot, function, PCI_CONFIG_INTERRUPT_LINE);
		device->interrupt_pin = pci_config_read8(bus, slot, function, PCI_CONFIG_INTERRUPT_PIN);

		if ((pci_config_read16(bus, slot, function, PCI_CONFIG_STATUS) & PCI_STATUS_CAPABILITIES) != 0U) {
			device->capabilities_pointer = (uint8_t)(pci_config_read8(bus, slot, function, PCI_CONFIG_CAPABILITIES) & 0xFCU);
		}
	}

	pci_size_bars(device);
	g_pci_device_count++;

	if (device->header_type == PCI_HEADER_TYPE_BRIDGE) {
		device->secondary_bus = pci_config_read8(bus, slot, function, PCI_CONFIG_SECONDARY_BUS);

		if (device->secondary_bus > bus && depth < PCI_MAX_SCAN_DEPTH) {
			pci_scan_bus(device->secondary_bus, depth + 1U);
		}
	}
}

static void pci_scan_bus(uint8_t bus, uint32_t depth)
{
	g_pci_bus_count++;

	for (uint8_t slot = 0U; slot < 32U; slot++) {
		if (pci_config_read16(bus, slot, 0U, PCI_CONFIG_VENDOR_ID) == PCI_VENDOR_INVALID) continue;

		uint8_t header = pci_config_read8(bus, slot, 0U, PCI_CONFIG_HEADER_TYPE);
		uint8_t functions = (header & PCI_HEADER_MULTIFUNCTION) != 0U ? 8U : 1U;

		for (uint8_t function = 0U; function < functions; function++) {
			pci_probe_function(bus, slot, function, depth);
		}
	}
}

bool pci_init(void)
{
	if (g_pci_initialized) return g_pci_present;

	g_pci_initialized = true;
	g_pci_present = pci_detect();

	if (!g_pci_present) {
		kputln("pci_init: no configuration mechanism 1 host bridge");
		return false;
	}

	pci_scan_bus(0U, 0U);
	return true;
}

bool pci_present(void)
{
	return g_pci_present;
}

uint32_t pci_bus_count(void)
{
	return g_pci_bus_count;
}

uint32_t pci_device_count(void)
{
	return g_pci_device_count;
}

const pci_device_t *pci_device_at(uint32_t index)
{
	return index < g_pci_device_count ? &g_pci_devices[index] : 0;
}

const pci_device_t *pci_find(uint16_t vendor_id, uint16_t device_id, uint32_t index)
{
	for (uint32_t position = 0U; position < g_pci_device_count; position++) {
		const pci_device_t *device = &g_pci_devices[position];

		if (device->vendor_id != vendor_id || device->device_id != device_id) continue;
		if (index-- == 0U) return device;
	}

	return 0;
}

const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass, uint32_t index)
{
	for (uint32_t position = 0U; position < g_pci_device_count; position++) {
		const pci_device_t *device = &g_pci_devices[position];

		if (device->class_code != class_code || device->subclass != subclass) continue;
		if (index-- == 0U) return device;
	}

	return 0;
}

bool pci_capability_next(const pci_device_t *device, uint8_t previous, uint8_t *id, uint8_t *offset)
{
	if (device == 0 || id == 0 || offset == 0 || device->capabilities_pointer == 0U) return false;

	uint8_t position;

	if (previous == 0U) {
		position = device->capabilities_pointer;
	} else {
		position = (uint8_t)(pci_config_read8(device->bus, device->slot, device->function, (uint8_t)(previous + 1U)) & 0xFCU);
	}

	/* A well-formed list is shorter than the 192 dwords of capability space. */
	if (position < 0x40U) return false;

	*offset = position;
	*id = pci_config_read8(device->bus, device->slot, device->function, position);
	return true;
}

void pci_command_update(const pci_device_t *device, uint16_t bits, bool set)
{
	if (device == 0) return;

	uint16_t command = pci_config_read16(device->bus, device->slot, device->function, PCI_CONFIG_COMMAND);

	command = set ? (uint16_t)(command | bits) : (uint16_t)(command & ~bits);
	pci_config_write16(device->bus, device->slot, device->function, PCI_CONFIG_COMMAND, command);
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
	switch (((uint32_t)class_code << 8U) | subclass) {
	case 0x0000U: return "unclassified";
	case 0x0100U: return "SCSI storage";
	case 0x0101U: return "IDE controller";
	case 0x0106U: return "SATA controller";
	case 0x0180U: return "mass storage";
	case 0x0200U: return "Ethernet controller";
	case 0x0300U: return "VGA controller";
	case 0x0380U: return "display controller";
	case 0x0600U: return "host bridge";
	case 0x0601U: return "ISA bridge";
	case 0x0604U: return "PCI bridge";
	case 0x0680U: return "bridge";
	case 0x0900U: return "keyboard";
	case 0x0902U: return "mouse";
	case 0x0980U: return "input device";
	default: return "other";
	}
}

volatile void *pci_map_bar(const pci_device_t *device, uint32_t bar, uint64_t offset, uint64_t length)
{
	if (device == 0 || bar >= PCI_MAX_BARS || !device->bars[bar].valid || device->bars[bar].io) return 0;

	const pci_bar_t *region = &device->bars[bar];

	if (region->base == 0ULL || offset > region->size || length > region->size - offset) return 0;

	uint64_t physical = region->base + offset;

	if (i386_mmio_map != 0) return i386_mmio_map(physical, length);

	uint32_t cr0;

	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));

	/* Paging off: physical == virtual, provided it fits the address space. */
	if ((cr0 & 0x80000000U) == 0U && physical + length <= 0x100000000ULL) {
		return (volatile void *)(uintptr_t)physical;
	}

	return 0;
}

void pci_print_location(const pci_device_t *device)
{
	kputhex_byte(device->bus);
	kputc(':');
	kputhex_byte(device->slot);
	kputc('.');
	kputhex_digit(device->function);
}

void pci_dump(void)
{
	kprintf("pci_dump: %u function(s) on %u bus(es)\n", g_pci_device_count, g_pci_bus_count);

	for (uint32_t index = 0U; index < g_pci_device_count; index++) {
		const pci_device_t *device = &g_pci_devices[index];

		kputs("pci_dump: ");
		pci_print_location(device);
		kprintf(
			" %x:%x class %x:%x (%s) irq %u pin %c\n",
			(unsigned int)device->vendor_id,
			(unsigned int)device->device_id,
			(unsigned int)device->class_code,
			(unsigned int)device->subclass,
			pci_class_name(device->class_code, device->subclass),
			(unsigned int)device->interrupt_line,
			device->interrupt_pin == 0U ? '-' : (char)('A' + device->interrupt_pin - 1U)
		);

		for (uint32_t bar = 0U; bar < PCI_MAX_BARS; bar++) {
			const pci_bar_t *region = &device->bars[bar];

			if (!region->valid) continue;

			kputs("pci_dump: ");
			pci_print_location(device);
			kprintf(
				" BAR%u %s%s base 0x%llx size 0x%llx\n",
				bar,
				region->io ? "io" : "mem",
				region->is64 ? "64" : "",
				(unsigned long long)region->base,
				(unsigned long long)region->size
			);
		}
	}
}
