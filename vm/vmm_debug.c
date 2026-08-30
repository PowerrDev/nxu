#include <kern/console/console.h>
#include <vm/vmm_internal.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>

/* Diagnostics and self-validation. Nothing here participates in mapping. */

static uint64_t vmm_kernel_symbol_physical(const void *symbol)
{
	uint64_t physical_address = 0ULL;

	if (!vmm_kernel_address_to_physical((uint64_t)symbol, &physical_address)) {
		return UINT64_MAX;
	}

	return physical_address;
}

static bool vmm_validate_read_only_section(uint64_t start, uint64_t end)
{
	if (start == end) return true;

	uint64_t expected_address;
	uint64_t physical_address;

	if (!vmm_kernel_address_to_physical(start, &expected_address)) {
		return false;
	}

	if (
		!vmm_translate(start, &physical_address) ||
		physical_address != expected_address
	) {
		return false;
	}

	return !vmm_translate_write(start, &physical_address);
}

static bool vmm_validate_writable_section(uint64_t start, uint64_t end)
{
	if (start == end) return true;

	uint64_t expected_address;
	uint64_t physical_address;

	if (!vmm_kernel_address_to_physical(start, &expected_address)) {
		return false;
	}

	if (!vmm_translate_write(start, &physical_address)) return false;

	return physical_address == expected_address;
}

bool vmm_validate_kernel_permissions(void)
{
	if (!g_vmm.enabled) return false;

	if (!vmm_validate_read_only_section(
		(uint64_t)__text_start,
		(uint64_t)__text_end
	)) {
		return false;
	}

	if (!vmm_validate_read_only_section(
		(uint64_t)__rodata_start,
		(uint64_t)__rodata_end
	)) {
		return false;
	}

	if (!vmm_validate_writable_section(
		(uint64_t)__data_start,
		(uint64_t)__data_end
	)) {
		return false;
	}

	return vmm_validate_writable_section(
		(uint64_t)__bss_start,
		(uint64_t)__bss_end
	);
}

static bool vmm_validate_linked_section(
	uint64_t start,
	uint64_t end,
	bool writable
)
{
	if (start == end) return true;

	if (
		!vmm_is_higher_half_address(start) ||
		!vmm_is_higher_half_address(end - 1ULL)
	) {
		return false;
	}

	uint64_t expected_address;
	uint64_t physical_address;

	if (!vmm_kernel_address_to_physical(start, &expected_address)) {
		return false;
	}

	if (
		!vmm_translate(start, &physical_address) ||
		physical_address != expected_address
	) {
		return false;
	}

	bool write_translation = vmm_translate_write(start, &physical_address);

	if (write_translation != writable) return false;

	return !write_translation || physical_address == expected_address;
}

bool vmm_validate_linked_kernel_layout(void)
{
	if (!g_vmm.higher_half_enabled) return false;

	if (!vmm_validate_linked_section(
		(uint64_t)__text_start,
		(uint64_t)__text_end,
		false
	)) {
		return false;
	}

	if (!vmm_validate_linked_section(
		(uint64_t)__rodata_start,
		(uint64_t)__rodata_end,
		false
	)) {
		return false;
	}

	if (!vmm_validate_linked_section(
		(uint64_t)__data_start,
		(uint64_t)__data_end,
		true
	)) {
		return false;
	}

	return vmm_validate_linked_section(
		(uint64_t)__bss_start,
		(uint64_t)__bss_end,
		true
	);
}

static bool vmm_validate_higher_alias(
	uint64_t physical_address,
	bool writable
)
{
	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(physical_address, &virtual_address)) {
		return false;
	}

	uint64_t translated_address;

	if (!vmm_translate(virtual_address, &translated_address)) return false;

	if (translated_address != physical_address) return false;

	bool write_translation = vmm_translate_write(virtual_address, &translated_address);

	if (write_translation != writable) return false;

	return !write_translation || translated_address == physical_address;
}

bool vmm_validate_higher_half_direct_map(const platform_t *platform)
{
	if (platform == 0 || !g_vmm.higher_half_direct_map_enabled) return false;

	if (!vmm_validate_higher_alias(
		vmm_kernel_symbol_physical(__text_start),
		false
	)) {
		return false;
	}

	if (!vmm_validate_higher_alias(
		vmm_kernel_symbol_physical(__rodata_start),
		false
	)) {
		return false;
	}

	uint64_t data_start = vmm_kernel_symbol_physical(__data_start);
	uint64_t data_end = vmm_kernel_symbol_physical(__data_end);

	if (data_start != data_end && !vmm_validate_higher_alias(data_start, true)) {
		return false;
	}

	if (!vmm_validate_higher_alias(
		vmm_kernel_symbol_physical(__bss_start),
		true
	)) {
		return false;
	}

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		if (
			platform->memory_regions[index].size != 0ULL &&
			!vmm_validate_higher_alias(
				platform->memory_regions[index].base,
				true
			)
		) {
			return false;
		}
	}

	if (!vmm_validate_higher_alias(platform->uart.base, true)) return false;


	if (
		platform->fw_cfg.size != 0ULL &&
		!vmm_validate_higher_alias(platform->fw_cfg.base, true)
	) {
		return false;
	}

	if (!vmm_validate_higher_alias(platform->gic_distributor.base, true)) {
		return false;
	}

	if (!vmm_validate_higher_alias(platform->gic_redistributor.base, true)) {
		return false;
	}

	if (
		platform->pcie_ecam.size != 0ULL &&
		!vmm_validate_higher_alias(platform->pcie_ecam.base, true)
	) {
		return false;
	}

	for (uint32_t index = 0U; index < platform->virtio_mmio_count; index++) {
		if (!vmm_validate_higher_alias(
			platform->virtio_mmio[index].base,
			true
		)) {
			return false;
		}
	}

	return true;
}

static void vmm_dump_section(
	const char *name,
	uint64_t start,
	uint64_t end,
	const char *permissions
)
{
	kputs("vmm: ");
	kputs(name);
	kputs(": ");
	kputhex64(start);
	kputs("-");
	kputhex64(end);
	kputc(' ');
	kputln(permissions);
}

void vmm_dump(void)
{
	kputs("vmm: root table: ");
	kputhex64(g_vmm.root_physical);
	kputc('\n');

	kputs("vmm: translation tables: ");
	kputu64(g_vmm.table_count);
	kputc('\n');

	kputs("vmm: virtual address size: ");
	kputu64(VMM_VA_BITS);
	kputln(" bits");

	kputs("vmm: physical address size: ");
	kputu64(g_vmm.physical_bits);
	kputln(" bits");

	kputs("vmm: TTBR0_EL1: ");
	kputhex64(vmm_read_ttbr0());
	kputc('\n');

	kputs("vmm: TCR_EL1: ");
	kputhex64(vmm_read_tcr());
	kputc('\n');

	kputs("vmm: MAIR_EL1: ");
	kputhex64(vmm_read_mair());
	kputc('\n');

	kputs("vmm: SCTLR_EL1: ");
	kputhex64(vmm_read_sctlr());
	kputc('\n');

	kputs("vmm: stage-1 translation: ");
	kputln((vmm_read_sctlr() & VMM_SCTLR_M) != 0ULL ? "enabled" : "disabled");

	vmm_dump_section(".text", (uint64_t)__text_start, (uint64_t)__text_end, "r-x");
	vmm_dump_section(".rodata", (uint64_t)__rodata_start, (uint64_t)__rodata_end, "r--");
	vmm_dump_section(".data", (uint64_t)__data_start, (uint64_t)__data_end, "rw-");
	vmm_dump_section(".bss", (uint64_t)__bss_start, (uint64_t)__bss_end, "rw-");
}

void vmm_dump_higher_half(void)
{
	uint64_t ttbr1 = vmm_read_ttbr1();
	uint64_t tcr = vmm_read_tcr();

	kputs("vmm: TTBR1 root: ");
	kputhex64((uint64_t)g_vmm.ttbr1_root);
	kputc('\n');

	kputs("vmm: TTBR1 translation tables: ");
	kputu64(g_vmm.ttbr1_table_count);
	kputc('\n');

	kputs("vmm: higher-half base: ");
	kputhex64(VMM_HIGHER_HALF_BASE);
	kputc('\n');

	kputs("vmm: TTBR1_EL1: ");
	kputhex64(ttbr1);
	kputc('\n');

	kputs("vmm: TCR_EL1 after TTBR1: ");
	kputhex64(tcr);
	kputc('\n');

	kputs("vmm: TTBR1 translation: ");
	kputln((tcr & VMM_TCR_EPD1) ? "disabled" : "enabled");
}
