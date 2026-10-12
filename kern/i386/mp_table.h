/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/mp_table.h
 *
 * What tells the boot CPU how many CPUs exist and how to address each one's
 * Local APIC: ACPI's MADT (Multiple APIC Description Table), with the Intel
 * MultiProcessor Specification 1.4 table as a fallback for firmware that
 * offers no ACPI at all.
 *
 * NxuACPIPlatform supplies the validated MADT from its firmware inventory,
 * using XSDT/RSDT and either a UEFI-provided RSDP or the legacy BIOS scan.
 * The Intel MP table remains a fallback when no usable MADT is available.
 */

#ifndef NXU_KERN_I386_MP_TABLE_H
#define NXU_KERN_I386_MP_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#define MP_TABLE_MAX_CPUS 8U

typedef struct {
	uint8_t apic_id;
	bool is_bsp;
} mp_table_cpu_t;

typedef struct {
	bool present;

	/* Physical address of every CPU's Local APIC (one MMIO window, same address on every CPU). */
	uint32_t lapic_physical;

	uint32_t cpu_count;
	mp_table_cpu_t cpus[MP_TABLE_MAX_CPUS];
} mp_table_t;

/*
 * Scan the three regions the MP spec says the floating pointer structure can
 * be in (the EBDA, the last KiB of base memory, the BIOS ROM area) and parse
 * the configuration table it points to. False if none is found or the one
 * found does not check out (bad checksum, no config table, more than
 * MP_TABLE_MAX_CPUS processor entries listed -- the extras are silently
 * dropped instead, this port has no use for the exact "default configuration
 * without config table" MP 1.1 legacy case a handful of very old chipsets
 * used, so that is treated as "not present" too).
 */
bool mp_table_scan(mp_table_t *table);

#endif
