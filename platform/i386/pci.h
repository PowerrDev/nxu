/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/pci.h
 *
 * PCI enumeration and configuration access over configuration mechanism 1
 * (I/O ports 0xCF8/0xCFC), for the legacy PC platform.
 *
 * pci_init() walks the buses once and records every function it finds, with
 * identity, class, interrupt routing, sized BARs and the capability list
 * pointer. Everything after that is table lookups; nothing here maps memory.
 * Memory BARs can only be used through pci_map_bar(), which needs either paging
 * to still be off or a mapping hook from the VM area (see its comment).
 */

#ifndef NXU_PLATFORM_I386_PCI_H
#define NXU_PLATFORM_I386_PCI_H

#include <stdbool.h>
#include <stdint.h>

#define PCI_MAX_DEVICES 48U
#define PCI_MAX_BARS 6U

#define PCI_VENDOR_INVALID 0xFFFFU

#define PCI_CONFIG_VENDOR_ID 0x00U
#define PCI_CONFIG_DEVICE_ID 0x02U
#define PCI_CONFIG_COMMAND 0x04U
#define PCI_CONFIG_STATUS 0x06U
#define PCI_CONFIG_REVISION 0x08U
#define PCI_CONFIG_PROG_IF 0x09U
#define PCI_CONFIG_SUBCLASS 0x0AU
#define PCI_CONFIG_CLASS 0x0BU
#define PCI_CONFIG_HEADER_TYPE 0x0EU
#define PCI_CONFIG_BAR0 0x10U
#define PCI_CONFIG_SECONDARY_BUS 0x19U
#define PCI_CONFIG_SUBSYSTEM_VENDOR 0x2CU
#define PCI_CONFIG_SUBSYSTEM_ID 0x2EU
#define PCI_CONFIG_CAPABILITIES 0x34U
#define PCI_CONFIG_INTERRUPT_LINE 0x3CU
#define PCI_CONFIG_INTERRUPT_PIN 0x3DU

#define PCI_COMMAND_IO 0x0001U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_BUS_MASTER 0x0004U
#define PCI_COMMAND_INTX_DISABLE 0x0400U

#define PCI_STATUS_CAPABILITIES 0x0010U

#define PCI_CAPABILITY_VENDOR 0x09U

#define PCI_CLASS_MASS_STORAGE 0x01U
#define PCI_CLASS_NETWORK 0x02U
#define PCI_CLASS_DISPLAY 0x03U
#define PCI_CLASS_BRIDGE 0x06U
#define PCI_CLASS_INPUT 0x09U

typedef struct {
	uint64_t base;
	uint64_t size;
	bool valid;
	bool io;
	bool prefetchable;
	bool is64;
} pci_bar_t;

typedef struct {
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	uint8_t header_type;

	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_id;

	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t revision;

	uint8_t interrupt_line;
	uint8_t interrupt_pin;
	uint8_t secondary_bus;
	uint8_t capabilities_pointer;

	pci_bar_t bars[PCI_MAX_BARS];
} pci_device_t;

/*
 * pci_init:
 *
 * Detect configuration mechanism 1 and enumerate the buses. Safe to call more
 * than once. Returns false when no PCI host bridge answers.
 */
bool pci_init(void);

bool pci_present(void);
uint32_t pci_bus_count(void);
uint32_t pci_device_count(void);
const pci_device_t *pci_device_at(uint32_t index);

/* The index'th function with this vendor/device ID, or null. */
const pci_device_t *pci_find(uint16_t vendor_id, uint16_t device_id, uint32_t index);

/* The index'th function of this class/subclass, or null. */
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass, uint32_t index);

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint8_t value);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);

/*
 * pci_capability_next:
 *
 * Walk a function's capability list. Pass 0 to get the first capability, or a
 * previously returned config offset to get the one after it. Writes the
 * capability ID and the config-space offset; returns false at the end of the
 * list.
 */
bool pci_capability_next(const pci_device_t *device, uint8_t previous, uint8_t *id, uint8_t *offset);

/* Set (set=true) or clear bits in the command register. */
void pci_command_update(const pci_device_t *device, uint16_t bits, bool set);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/*
 * pci_map_bar:
 *
 * A CPU-accessible pointer to length bytes at offset inside a memory BAR, or
 * null. There is no MMIO mapping API yet, so this works while paging is still
 * off (the BAR's physical address is directly addressable if it lies below
 * 4 GiB) or when the VM area supplies the hook
 *
 *   void *i386_mmio_map(uint64_t physical, uint64_t size);
 *
 * as a strong symbol (mapping the range uncached into the device window and
 * returning its virtual address, or null). Without either, it returns null and
 * the caller must do without the device.
 */
volatile void *pci_map_bar(const pci_device_t *device, uint32_t bar, uint64_t offset, uint64_t length);

/* Print "bb:ss.f" for a function, without a newline. */
void pci_print_location(const pci_device_t *device);

void pci_dump(void);

#endif
