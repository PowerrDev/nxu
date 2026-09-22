/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/mp_table.c
 *
 * See mp_table.h.
 */

#include <kern/i386/mp_table.h>

#include <kern/i386/vm_param.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Everything scanned here lies below 1 MiB, comfortably inside the direct map. */
static inline const void *mp_direct(uint32_t physical_address)
{
	return (const void *)(uintptr_t)(physical_address + VMM_HIGHER_HALF_BASE);
}

/*
 * The same, for a physical address that came from table *content* (an RSDT
 * entry, an RSDP's rsdt_physical) rather than the fixed low-memory regions
 * this file scans directly: only [0, VM_DIRECT_MAP_SIZE) is mapped at all,
 * and firmware is not obligated to keep every table below 1 MiB the way the
 * RSDP itself always is. 0 (mp_direct(0) is never a valid table -- it is
 * where the real-mode IVT lives) means "outside the direct map, refuse it".
 */
static const void *mp_direct_checked(uint32_t physical_address)
{
	if (physical_address == 0U || (uint64_t)physical_address >= VM_DIRECT_MAP_SIZE) return 0;

	return mp_direct(physical_address);
}

typedef struct __attribute__((packed)) {
	char signature[4]; /* "_MP_" */
	uint32_t config_physical;
	uint8_t length; /* In 16-byte units; always 1. */
	uint8_t spec_rev; /* 1 = MP 1.1, 4 = MP 1.4. */
	uint8_t checksum;
	uint8_t feature[5];
} mp_floating_pointer_t;

_Static_assert(sizeof(mp_floating_pointer_t) == 16U, "MP floating pointer structure must be 16 bytes");

typedef struct __attribute__((packed)) {
	char signature[4]; /* "PCMP" */
	uint16_t length;
	uint8_t spec_rev;
	uint8_t checksum;
	char oem_id[8];
	char product_id[12];
	uint32_t oem_table_physical;
	uint16_t oem_table_size;
	uint16_t entry_count;
	uint32_t lapic_physical;
	uint16_t ext_table_length;
	uint8_t ext_table_checksum;
	uint8_t reserved;
} mp_config_header_t;

_Static_assert(sizeof(mp_config_header_t) == 44U, "MP configuration table header must be 44 bytes");

#define MP_ENTRY_PROCESSOR 0U
#define MP_ENTRY_BUS 1U
#define MP_ENTRY_IOAPIC 2U
#define MP_ENTRY_IO_INTERRUPT 3U
#define MP_ENTRY_LOCAL_INTERRUPT 4U

#define MP_ENTRY_SIZE_PROCESSOR 20U
/* Every non-processor entry type this port skips over is 8 bytes. */
#define MP_ENTRY_SIZE_SHORT 8U

typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t local_apic_id;
	uint8_t local_apic_version;
	uint8_t cpu_flags;
	uint32_t cpu_signature;
	uint32_t feature_flags;
	uint32_t reserved[2];
} mp_entry_processor_t;

_Static_assert(sizeof(mp_entry_processor_t) == MP_ENTRY_SIZE_PROCESSOR, "MP processor entry must be 20 bytes");

#define MP_CPU_FLAG_ENABLED 0x01U
#define MP_CPU_FLAG_BSP 0x02U

static uint8_t mp_checksum(const void *data, uint32_t length)
{
	const uint8_t *bytes = (const uint8_t *)data;
	uint8_t sum = 0U;

	for (uint32_t index = 0U; index < length; index++) sum = (uint8_t)(sum + bytes[index]);

	return sum;
}

/* The floating pointer structure, or 0 if [base, base + length) holds none. */
static const mp_floating_pointer_t *mp_scan_range(uint32_t base, uint32_t length)
{
	for (uint32_t offset = 0U; offset + sizeof(mp_floating_pointer_t) <= length; offset += 16U) {
		const mp_floating_pointer_t *candidate = (const mp_floating_pointer_t *)mp_direct(base + offset);

		if (memcmp(candidate->signature, "_MP_", 4U) != 0) continue;
		if (candidate->length != 1U) continue;
		if (mp_checksum(candidate, sizeof(*candidate)) != 0U) continue;

		return candidate;
	}

	return 0;
}

static const mp_floating_pointer_t *mp_find_floating_pointer(void)
{
	/* The EBDA segment (paragraphs), at the fixed BIOS data area word 0x40E. */
	uint16_t ebda_segment = *(const uint16_t *)mp_direct(0x40EU);

	if (ebda_segment != 0U) {
		uint32_t ebda_base = (uint32_t)ebda_segment << 4U;
		const mp_floating_pointer_t *found = mp_scan_range(ebda_base, 1024U);

		if (found != 0) return found;
	}

	/* The last KiB of base memory below the traditional 640 KiB boundary. */
	const mp_floating_pointer_t *found = mp_scan_range(0x9FC00U, 1024U);

	if (found != 0) return found;

	/* The BIOS ROM area. */
	return mp_scan_range(0xF0000U, 0x10000U);
}

/* ---- ACPI: RSDP -> RSDT -> MADT ------------------------------------------ */

typedef struct __attribute__((packed)) {
	char signature[8]; /* "RSD PTR " (note the trailing space) */
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_physical;
	/* ACPI 2.0+ fields follow (length, xsdt_physical, extended_checksum, reserved[3]); unused, this port stays on the RSDT. */
} acpi_rsdp_t;

_Static_assert(sizeof(acpi_rsdp_t) == 20U, "ACPI 1.0 RSDP must be 20 bytes");

typedef struct __attribute__((packed)) {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} acpi_table_header_t;

_Static_assert(sizeof(acpi_table_header_t) == 36U, "ACPI table header must be 36 bytes");

#define ACPI_MADT_ENTRY_LOCAL_APIC 0U

typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t length;
	uint8_t acpi_processor_id;
	uint8_t apic_id;
	uint32_t flags; /* bit 0: enabled */
} acpi_madt_local_apic_t;

_Static_assert(sizeof(acpi_madt_local_apic_t) == 8U, "ACPI MADT Processor Local APIC entry must be 8 bytes");

#define ACPI_MADT_LOCAL_APIC_ENABLED 0x00000001U

static const acpi_rsdp_t *acpi_find_rsdp(void)
{
	uint16_t ebda_segment = *(const uint16_t *)mp_direct(0x40EU);

	if (ebda_segment != 0U) {
		uint32_t ebda_base = (uint32_t)ebda_segment << 4U;

		for (uint32_t offset = 0U; offset + sizeof(acpi_rsdp_t) <= 1024U; offset += 16U) {
			const acpi_rsdp_t *candidate = (const acpi_rsdp_t *)mp_direct(ebda_base + offset);

			if (memcmp(candidate->signature, "RSD PTR ", 8U) == 0 && mp_checksum(candidate, sizeof(*candidate)) == 0U) return candidate;
		}
	}

	/* The BIOS ROM area, ACPI spec's other mandated search range. */
	for (uint32_t offset = 0U; offset + sizeof(acpi_rsdp_t) <= 0x20000U; offset += 16U) {
		const acpi_rsdp_t *candidate = (const acpi_rsdp_t *)mp_direct(0xE0000U + offset);

		if (memcmp(candidate->signature, "RSD PTR ", 8U) == 0 && mp_checksum(candidate, sizeof(*candidate)) == 0U) return candidate;
	}

	return 0;
}

/*
 * A real RSDT/MADT/etc. is at most a few KiB; this is a sanity bound, not a
 * spec limit, against a garbage `length` field (a bad RSDP checksum
 * false-positive, or simply a physical address that does not actually hold
 * the table it claims to) turning a length-driven loop into a walk off the
 * end of mapped memory. `length` must also be at least the common header's
 * own size, or `length - sizeof(header)` below underflows (both are
 * unsigned) into a huge entry count.
 */
#define ACPI_TABLE_LENGTH_MAX 8192U

static bool acpi_table_length_sane(uint32_t length)
{
	return length >= (uint32_t)sizeof(acpi_table_header_t) && length <= ACPI_TABLE_LENGTH_MAX;
}

/* The physical address of the table named `signature` (4 bytes, not nul-terminated) that the RSDT lists, or 0. */
static uint32_t acpi_find_table(const acpi_table_header_t *rsdt, const char *signature)
{
	uint32_t entry_count = (rsdt->length - (uint32_t)sizeof(*rsdt)) / 4U;
	const uint32_t *entries = (const uint32_t *)((const uint8_t *)rsdt + sizeof(*rsdt));

	for (uint32_t index = 0U; index < entry_count; index++) {
		const acpi_table_header_t *candidate = (const acpi_table_header_t *)mp_direct_checked(entries[index]);

		if (candidate != 0 && memcmp(candidate->signature, signature, 4U) == 0) return entries[index];
	}

	return 0U;
}

/* True if a usable MADT was found and table->cpu_count/lapic_physical are filled in. */
static bool acpi_scan(mp_table_t *table)
{
	const acpi_rsdp_t *rsdp = acpi_find_rsdp();

	if (rsdp == 0) {
		kputln("mp_table_scan: no ACPI RSDP found");
		return false;
	}

	kprintf("mp_table_scan: ACPI RSDP at phys 0x%x, revision %u\n", (uint32_t)((uintptr_t)rsdp - VMM_HIGHER_HALF_BASE), rsdp->revision);

	const acpi_table_header_t *rsdt = (const acpi_table_header_t *)mp_direct_checked(rsdp->rsdt_physical);

	if (rsdt == 0 || memcmp(rsdt->signature, "RSDT", 4U) != 0 || !acpi_table_length_sane(rsdt->length)) {
		kputln("mp_table_scan: ACPI RSDT signature mismatch or implausible length");
		return false;
	}

	if (mp_checksum(rsdt, rsdt->length) != 0U) {
		kputln("mp_table_scan: ACPI RSDT checksum mismatch");
		return false;
	}

	uint32_t madt_physical = acpi_find_table(rsdt, "APIC");

	if (madt_physical == 0U) {
		kputln("mp_table_scan: ACPI RSDT lists no MADT");
		return false;
	}

	const acpi_table_header_t *madt_header = (const acpi_table_header_t *)mp_direct_checked(madt_physical);

	if (madt_header == 0 || !acpi_table_length_sane(madt_header->length)) {
		kputln("mp_table_scan: ACPI MADT outside the direct map or implausible length");
		return false;
	}

	if (mp_checksum(madt_header, madt_header->length) != 0U) {
		kputln("mp_table_scan: ACPI MADT checksum mismatch");
		return false;
	}

	/* Fixed MADT fields right after the common header: local_apic_address(u32), flags(u32). */
	uint32_t lapic_physical = *(const uint32_t *)((const uint8_t *)madt_header + sizeof(*madt_header));

	const uint8_t *cursor = (const uint8_t *)madt_header + sizeof(*madt_header) + 8U;
	const uint8_t *end = (const uint8_t *)madt_header + madt_header->length;

	while (cursor + 2U <= end) {
		uint8_t entry_type = cursor[0];
		uint8_t entry_length = cursor[1];

		if (entry_length < 2U || cursor + entry_length > end) break;

		if (entry_type == ACPI_MADT_ENTRY_LOCAL_APIC && entry_length >= sizeof(acpi_madt_local_apic_t)) {
			const acpi_madt_local_apic_t *entry = (const acpi_madt_local_apic_t *)cursor;

			if ((entry->flags & ACPI_MADT_LOCAL_APIC_ENABLED) != 0U && table->cpu_count < MP_TABLE_MAX_CPUS) {
				table->cpus[table->cpu_count].apic_id = entry->apic_id;
				table->cpu_count++;
			}
		}

		cursor += entry_length;
	}

	table->lapic_physical = lapic_physical;

	kprintf("mp_table_scan: ACPI MADT lists %u CPU(s), Local APIC at phys 0x%x\n", table->cpu_count, table->lapic_physical);

	for (uint32_t index = 0U; index < table->cpu_count; index++) {
		kprintf("mp_table_scan: cpu apic_id=%u\n", table->cpus[index].apic_id);
	}

	/*
	 * MADT has no per-CPU "this one is the BSP" flag the way the legacy MP
	 * table does: the running CPU's own Local APIC id (read once the Local
	 * APIC is mapped) is what smp_boot_secondaries() matches against this
	 * list to find its own entry, not a flag here.
	 */
	table->present = table->cpu_count != 0U;
	return table->present;
}

bool mp_table_scan(mp_table_t *table)
{
	if (table == 0) return false;

	memset(table, 0, sizeof(*table));

	if (acpi_scan(table)) return true;

	memset(table, 0, sizeof(*table));

	const mp_floating_pointer_t *floating_pointer = mp_find_floating_pointer();

	if (floating_pointer == 0) {
		kputln("mp_table_scan: no MP floating pointer structure found");
		return false;
	}

	kprintf(
		"mp_table_scan: floating pointer at phys 0x%x, spec 1.%u\n",
		(uint32_t)((uintptr_t)floating_pointer - VMM_HIGHER_HALF_BASE),
		floating_pointer->spec_rev
	);

	if (floating_pointer->feature[0] != 0U || floating_pointer->config_physical == 0U) {
		/* MP 1.1's "default configuration" case: no config table to read the real topology from. */
		kputln("mp_table_scan: default configuration (no MP configuration table), treating as uniprocessor");
		return false;
	}

	const mp_config_header_t *header = (const mp_config_header_t *)mp_direct_checked(floating_pointer->config_physical);

	if (header == 0 || memcmp(header->signature, "PCMP", 4U) != 0) {
		kputln("mp_table_scan: MP configuration table outside the direct map or signature mismatch");
		return false;
	}

	if (header->length < (uint16_t)sizeof(*header) || header->length > (uint16_t)ACPI_TABLE_LENGTH_MAX) {
		kputln("mp_table_scan: MP configuration table implausible length");
		return false;
	}

	if (mp_checksum(header, header->length) != 0U) {
		kputln("mp_table_scan: MP configuration table checksum mismatch");
		return false;
	}

	table->lapic_physical = header->lapic_physical;

	const uint8_t *cursor = (const uint8_t *)header + sizeof(*header);
	const uint8_t *end = (const uint8_t *)header + header->length;

	for (uint16_t index = 0U; index < header->entry_count && cursor < end; index++) {
		uint8_t entry_type = cursor[0];

		if (entry_type == MP_ENTRY_PROCESSOR) {
			const mp_entry_processor_t *processor = (const mp_entry_processor_t *)cursor;

			if ((processor->cpu_flags & MP_CPU_FLAG_ENABLED) != 0U && table->cpu_count < MP_TABLE_MAX_CPUS) {
				table->cpus[table->cpu_count].apic_id = processor->local_apic_id;
				table->cpus[table->cpu_count].is_bsp = (processor->cpu_flags & MP_CPU_FLAG_BSP) != 0U;
				table->cpu_count++;
			}

			cursor += MP_ENTRY_SIZE_PROCESSOR;
		} else if (entry_type == MP_ENTRY_BUS || entry_type == MP_ENTRY_IOAPIC ||
			entry_type == MP_ENTRY_IO_INTERRUPT || entry_type == MP_ENTRY_LOCAL_INTERRUPT) {
			/*
			 * This port keeps the legacy 8259 PIC for every existing device
			 * IRQ (see doc/i386/smp.md's known gaps): bus, IOAPIC and
			 * interrupt-routing entries are skipped, not parsed.
			 */
			cursor += MP_ENTRY_SIZE_SHORT;
		} else {
			/* An unrecognised entry type: the table is malformed from here on, stop. */
			break;
		}
	}

	kprintf(
		"mp_table_scan: %u CPU(s), Local APIC at phys 0x%x\n",
		table->cpu_count,
		table->lapic_physical
	);

	for (uint32_t index = 0U; index < table->cpu_count; index++) {
		kprintf(
			"mp_table_scan: cpu apic_id=%u%s\n",
			table->cpus[index].apic_id,
			table->cpus[index].is_bsp ? " (BSP)" : ""
		);
	}

	table->present = table->cpu_count != 0U;
	return table->present;
}
