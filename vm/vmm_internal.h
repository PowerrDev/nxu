#ifndef NXU_VM_VMM_INTERNAL_H
#define NXU_VM_VMM_INTERNAL_H

#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Private interface shared by vmm.c, vmm_tables.c, vmm_ttbr1.c and
 * vmm_debug.c. No subsystem outside vm/ may include this header.
 */

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

extern uint8_t __text_start[];
extern uint8_t __text_end[];

extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];

extern uint8_t __data_start[];
extern uint8_t __data_end[];

extern uint8_t __bss_start[];
extern uint8_t __bss_end[];

#define VMM_VA_BITS 39U

#define VMM_TABLE_ENTRY_COUNT 512ULL
#define VMM_INDEX_MASK 0x1FFULL

#define VMM_L1_SHIFT 30U
#define VMM_L2_SHIFT 21U
#define VMM_L3_SHIFT 12U

#define VMM_L1_SIZE (1ULL << VMM_L1_SHIFT)
#define VMM_L2_SIZE (1ULL << VMM_L2_SHIFT)
#define VMM_L3_SIZE (1ULL << VMM_L3_SHIFT)

#define VMM_DESC_VALID (1ULL << 0U)
#define VMM_DESC_TABLE (1ULL << 1U)
#define VMM_DESC_AF (1ULL << 10U)
/*
 * Bits 58:55 of a stage-1 descriptor are reserved for software. NXU marks a
 * page that is shared copy-on-write between address spaces with bit 55: the
 * descriptor is read-only in hardware, and the first write fault gives the
 * writer a private copy (vm_address_space_cow_break).
 */
#define VMM_DESC_SW_COW (1ULL << 55U)
#define VMM_DESC_PXN (1ULL << 53U)
#define VMM_DESC_UXN (1ULL << 54U)

#define VMM_DESC_BLOCK VMM_DESC_VALID
#define VMM_DESC_TABLE_PAGE (VMM_DESC_VALID | VMM_DESC_TABLE)

#define VMM_DESC_TYPE_MASK 0x3ULL
#define VMM_DESC_ATTR_MASK (7ULL << 2U)
#define VMM_DESC_AP_MASK (3ULL << 6U)

#define VMM_DESC_ADDRESS_MASK 0x0000FFFFFFFFF000ULL

/*
 * AP[2:1] = 0b10:
 *
 * EL1: read-only
 * EL0: no access
 */
#define VMM_DESC_AP_READ_ONLY (2ULL << 6U)

#define VMM_DESC_ATTR(index) ((uint64_t)(index) << 2U)

#define VMM_DESC_SH_OUTER (2ULL << 8U)
#define VMM_DESC_SH_INNER (3ULL << 8U)

#define VMM_MAIR_NORMAL_INDEX 0U
#define VMM_MAIR_DEVICE_INDEX 1U

#define VMM_MAIR_NORMAL_WB 0xFFULL
#define VMM_MAIR_DEVICE_NGNRE 0x04ULL

#define VMM_TCR_T0SZ(value) ((uint64_t)(value) << 0U)
#define VMM_TCR_EPD0 (1ULL << 7U)
#define VMM_TCR_IRGN0_WBWA (1ULL << 8U)
#define VMM_TCR_ORGN0_WBWA (1ULL << 10U)
#define VMM_TCR_SH0_INNER (3ULL << 12U)
#define VMM_TCR_TG0_4K (0ULL << 14U)

#define VMM_TCR_T1SZ(value) ((uint64_t)(value) << 16U)
#define VMM_TCR_EPD1 (1ULL << 23U)
#define VMM_TCR_IRGN1_WBWA (1ULL << 24U)
#define VMM_TCR_ORGN1_WBWA (1ULL << 26U)
#define VMM_TCR_SH1_INNER (3ULL << 28U)
#define VMM_TCR_TG1_4K (2ULL << 30U)

#define VMM_TCR_IPS(value) ((uint64_t)(value) << 32U)

#define VMM_SCTLR_M (1ULL << 0U)
#define VMM_SCTLR_WXN (1ULL << 19U)

#define VMM_VA_LOW_MASK ((1ULL << VMM_VA_BITS) - 1ULL)
#define VMM_VA_HIGH_MASK (~VMM_VA_LOW_MASK)

typedef struct {
	uint64_t *root;
	uint64_t root_physical;
	uint64_t table_count;

	uint64_t *ttbr1_root;
	uint64_t ttbr1_root_physical;
	uint64_t ttbr1_table_count;

	uint32_t physical_bits;
	uint32_t ips;

	bool enabled;
	bool higher_half_enabled;
	bool higher_half_direct_map_enabled;
	bool kernel_pointers_rebased;
	bool ttbr0_disabled;
} vmm_state_t;

bool vmm_root_map_range(
	uint64_t *root,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection,
	uint64_t *table_count
);

/*
 * Defined once in vmm.c. The symbol stays internal to the VMM because
 * only this header declares it.
 */
extern vmm_state_t g_vmm;

uint64_t vmm_index(uint64_t address, uint32_t shift);

/*
 * Convert a physical translation-table address into the kernel pointer
 * appropriate for the current boot phase.
 */
bool vmm_table_pointer_from_physical(
	uint64_t physical_address,
	uint64_t **table
);

/*
 * Allocate one zeroed translation table and account for it in the
 * counter belonging to the owning root.
 */
bool vmm_allocate_table(
	uint64_t **table,
	uint64_t *physical,
	uint64_t *table_count
);

bool vmm_attributes(
	vmm_memory_type_t type,
	vmm_protection_t protection,
	uint64_t *attributes
);

bool vmm_physical_page_valid(uint64_t physical_address);
bool vmm_lower_page_valid(uint64_t virtual_address);
bool vmm_higher_page_valid(uint64_t virtual_address);

bool vmm_make_page_descriptor(
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection,
	uint64_t *descriptor
);

bool vmm_descriptor_memory_type(
	uint64_t descriptor,
	vmm_memory_type_t *memory_type
);

bool vmm_descriptor_protection(
	uint64_t descriptor,
	vmm_protection_t *protection
);

/*
 * Walk any root down to the L3 entry describing virtual_address.
 *
 * When create is true, missing L1 and L2 tables are allocated and
 * charged to table_count. Block descriptors are never split.
 */
bool vmm_root_get_l3_entry(
	uint64_t *root,
	uint64_t virtual_address,
	bool create,
	uint64_t *table_count,
	uint64_t **entry
);

/*
 * Shared block/page installers used while building the initial TTBR0
 * identity map. They operate on g_vmm.root.
 */
bool vmm_map_initial_l1(uint64_t address, uint64_t attributes);
bool vmm_map_initial_l2(uint64_t address, uint64_t attributes);
bool vmm_map_initial_l3(uint64_t address, uint64_t attributes);

void vmm_publish_new_mapping(void);
void vmm_invalidate_page(uint64_t virtual_address);

uint64_t vmm_read_sctlr(void);
uint64_t vmm_read_ttbr0(void);
uint64_t vmm_read_ttbr1(void);
uint64_t vmm_read_tcr(void);
uint64_t vmm_read_mair(void);

#endif
