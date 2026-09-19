/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/platform.c
 *
 * Boot platform discovery for the legacy PC: the same platform_bootstrap() /
 * platform_get() / platform_dump() interface the arm64 build implements from a
 * Device Tree, built here from what a PC always has.
 *
 *   memory   the Multiboot memory map, loaded by the VM area's
 *            i386_memory_map_load() (weak here: without it the map is simply
 *            reported as unavailable, so this area builds and boots alone)
 *   uart     COM1, I/O ports 0x3F8-0x3FF
 *   rtc      CMOS RTC, index/data ports 0x70-0x71
 *   pci      configuration mechanism 1, see pci.h
 *
 * There is no Device Tree: the dtb argument is accepted for interface
 * compatibility and ignored.
 */

#include <platform/platform.h>
#include <platform/i386/pci.h>

#include <kern/console/console.h>
#include <mach/i386/boot_info.h>
#include <mach/i386/multiboot.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PLATFORM_COM1_BASE 0x3F8ULL
#define PLATFORM_COM1_PORTS 8ULL
#define PLATFORM_CMOS_BASE 0x70ULL
#define PLATFORM_CMOS_PORTS 2ULL

/*
 * The VM area owns memory_map.{h,c}. Declared here rather than included so
 * the devices area does not depend on a header that may not exist yet; a null
 * address means the map is not available.
 */
extern bool i386_memory_map_load(platform_t *platform, const multiboot_info_t *mbi) __attribute__((weak));

static platform_t g_platform;
static bool g_platform_ready;

bool platform_discover(const dtb_t *dtb, platform_t *platform)
{
	(void)dtb;

	if (platform == 0) return false;

	memset(platform, 0, sizeof(*platform));

	platform->uart.base = PLATFORM_COM1_BASE;
	platform->uart.size = PLATFORM_COM1_PORTS;
	platform->rtc.base = PLATFORM_CMOS_BASE;
	platform->rtc.size = PLATFORM_CMOS_PORTS;

	const i386_boot_info_t *boot = i386_boot_info();
	const multiboot_info_t *multiboot = 0;

	if (boot != 0 && boot->magic == MULTIBOOT_BOOTLOADER_MAGIC) multiboot = boot->multiboot;

	if (i386_memory_map_load == 0) {
		kputln("platform_discover: memory map unavailable (i386_memory_map_load not linked)");
	} else if (!i386_memory_map_load(platform, multiboot)) {
		kputln("platform_discover: memory map load failed");
		return false;
	}

	if (pci_init()) {
		platform->pci_bus_count = pci_bus_count();
		platform->pci_device_count = pci_device_count();
	} else {
		kputln("platform_discover: no PCI bus");
	}

	return true;
}

bool platform_bootstrap(const dtb_t *dtb)
{
	if (g_platform_ready) return false;

	if (!platform_discover(dtb, &g_platform)) {
		memset(&g_platform, 0, sizeof(g_platform));
		return false;
	}

	g_platform_ready = true;
	return true;
}

const platform_t *platform_get(void)
{
	return g_platform_ready ? &g_platform : 0;
}

void platform_dump(const platform_t *platform)
{
	if (platform == 0) return;

	kprintf("platform_dump: memory regions: %u\n", platform->memory_region_count);

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		kprintf(
			"platform_dump: RAM[%u] base: 0x%llx, size: %llu bytes\n",
			index,
			(unsigned long long)platform->memory_regions[index].base,
			(unsigned long long)platform->memory_regions[index].size
		);
	}

	kprintf(
		"platform_dump: UART (COM1) I/O ports 0x%llx-0x%llx\n",
		(unsigned long long)platform->uart.base,
		(unsigned long long)(platform->uart.base + platform->uart.size - 1ULL)
	);

	kprintf(
		"platform_dump: RTC (CMOS) I/O ports 0x%llx-0x%llx\n",
		(unsigned long long)platform->rtc.base,
		(unsigned long long)(platform->rtc.base + platform->rtc.size - 1ULL)
	);

	kprintf(
		"platform_dump: PCI: %u function(s) on %u bus(es)\n",
		platform->pci_device_count,
		platform->pci_bus_count
	);

	if (platform->pci_device_count != 0U) pci_dump();
}
