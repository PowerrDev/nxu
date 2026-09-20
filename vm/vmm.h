#ifndef NXU_VMM_H
#define NXU_VMM_H

#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

#include <kern/machine/vm_param.h>

typedef enum {
	VMM_MEMORY_NORMAL,
	VMM_MEMORY_DEVICE
} vmm_memory_type_t;

typedef enum {
	VMM_PROTECTION_READ_WRITE,
	VMM_PROTECTION_READ_ONLY,
	VMM_PROTECTION_READ_EXECUTE
} vmm_protection_t;

typedef struct {
	uint64_t physical_address;

	vmm_memory_type_t memory_type;
	vmm_protection_t protection;
} vmm_page_mapping_t;

/*
 * Build the initial identity-mapped address space and enable
 * EL1 stage-1 translation.
 */
bool vmm_init(const platform_t *platform);

/*
 * Add an identity mapping while the MMU is still disabled.
 *
 * This remains an early-boot interface. Use vmm_map_page() for
 * live mappings after vmm_init().
 */
bool vmm_map_identity(
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
);

/*
 * Install one live 4 KiB page mapping.
 *
 * Both addresses must be page-aligned. The virtual address must
 * currently be unmapped.
 */
bool vmm_map_page(
	uint64_t virtual_address,
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
);

/*
 * Remove one live 4 KiB page mapping.
 *
 * If physical_address is not null, it receives the old physical
 * page address.
 */
bool vmm_unmap_page(
	uint64_t virtual_address,
	uint64_t *physical_address
);

/*
 * Change the permissions of an existing L3 page mapping using
 * break-before-make.
 */
bool vmm_protect_page(
	uint64_t virtual_address,
	vmm_protection_t protection
);

/*
 * Read the descriptor for an existing L3 page mapping.
 */
bool vmm_query_page(
	uint64_t virtual_address,
	vmm_page_mapping_t *mapping
);

/*
 * Test an EL1 read translation through AT S1E1R.
 */
bool vmm_translate(
	uint64_t virtual_address,
	uint64_t *physical_address
);

/*
 * Test an EL1 write translation through AT S1E1W.
 */
bool vmm_translate_write(
	uint64_t virtual_address,
	uint64_t *physical_address
);

bool vmm_validate_kernel_permissions(void);
bool vmm_validate_linked_kernel_layout(void);

bool vmm_is_enabled(void);

void vmm_dump(void);

/**
 * vmm_init_higher_half_alias - Install the permanent kernel TTBR1 mapping.
 *
 * The ELF is linked at VMM_HIGHER_HALF_BASE plus its physical load address.
 * This function builds the TTBR1 hierarchy that makes those linked virtual
 * addresses valid while bootstrap execution still uses the physical alias.
 *
 * Return: true on success, otherwise false.
 */
bool vmm_init_higher_half_alias(void);

/**
 * vmm_physical_to_higher_half - Convert a physical address to its
 * higher-half direct-map alias.
 * @physical_address: Physical address to convert.
 * @virtual_address: Receives the higher-half address.
 *
 * Return: true when the address fits in the configured 39-bit region.
 */
bool vmm_physical_to_higher_half(
	uint64_t physical_address,
	uint64_t *virtual_address
);

/**
 * vmm_higher_half_to_physical - Convert a direct-map alias back to PA.
 * @virtual_address: Higher-half direct-map address.
 * @physical_address: Receives the physical address.
 *
 * Return: true when @virtual_address belongs to the higher-half region.
 */
bool vmm_higher_half_to_physical(
	uint64_t virtual_address,
	uint64_t *physical_address
);

/**
 * vmm_kernel_address_to_physical - Resolve either kernel alias to its PA.
 * @kernel_address: Kernel address from the bootstrap or permanent alias.
 * @physical_address: Receives the corresponding physical address.
 *
 * During bootstrap, PC-relative references produce the physical alias. After
 * the TTBR1 transition the same linked symbols resolve in the higher half.
 * This helper accepts either representation and returns the common PA.
 */
bool vmm_kernel_address_to_physical(
	uint64_t kernel_address,
	uint64_t *physical_address
);

bool vmm_is_higher_half_address(
	uint64_t virtual_address
);

bool vmm_higher_half_enabled(void);

/**
 * vmm_map_higher_half_direct_map - Map platform memory through TTBR1.
 * @platform: Discovered platform resources.
 *
 * Extends the existing TTBR1 kernel alias to include RAM and MMIO.
 *
 * Return: true on success, otherwise false.
 */
bool vmm_map_higher_half_direct_map(const platform_t *platform);

/**
 * vmm_validate_higher_half_direct_map - Validate TTBR1 platform mappings.
 * @platform: Discovered platform resources.
 *
 * Return: true when the expected higher-half translations and permissions
 * are active, otherwise false.
 */
bool vmm_validate_higher_half_direct_map(const platform_t *platform);

bool vmm_higher_half_direct_map_enabled(void);

/**
 * vmm_rebase_kernel_pointers - Move software table pointers to TTBR1.
 *
 * The hardware TTBR values remain physical. Only the C pointers used to
 * edit translation tables are changed to higher-half direct-map aliases.
 */
bool vmm_rebase_kernel_pointers(void);

/**
 * vmm_disable_ttbr0 - Disable lower-address translation-table walks.
 *
 * Requires permanent higher-half execution and rebased kernel pointers.
 */
bool vmm_disable_ttbr0(void);

bool vmm_ttbr0_disabled(void);

uint64_t vmm_get_ttbr1_root(void);
uint64_t vmm_get_ttbr1_table_count(void);

void vmm_dump_higher_half(void);

#endif
