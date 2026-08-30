#include <platform/fw_cfg.h>

#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FW_CFG_DATA_OFFSET 0x00ULL
#define FW_CFG_SELECTOR_OFFSET 0x08ULL
#define FW_CFG_DMA_OFFSET 0x10ULL
#define FW_CFG_MIN_SIZE 0x18ULL

#define FW_CFG_SIGNATURE 0x0000U
#define FW_CFG_ID 0x0001U
#define FW_CFG_FILE_DIR 0x0019U

#define FW_CFG_ID_DMA (1U << 1)

#define FW_CFG_DMA_CTL_ERROR 0x01U
#define FW_CFG_DMA_CTL_SELECT 0x08U
#define FW_CFG_DMA_CTL_WRITE 0x10U

#define FW_CFG_DMA_BOUNCE_OFFSET 64U
#define FW_CFG_FILE_NAME_SIZE 56U
#define FW_CFG_MAX_FILES 256U

/*
 * fw_cfg MMIO control registers and DMA descriptors are big-endian even on
 * QEMU's little-endian AArch64 virt machine.
 */
static uint16_t fw_cfg_to_be16(uint16_t value)
{
	return (uint16_t)((value >> 8U) | (value << 8U));
}

static uint32_t fw_cfg_to_be32(uint32_t value)
{
	return ((value & 0x000000FFU) << 24U)
		| ((value & 0x0000FF00U) << 8U)
		| ((value & 0x00FF0000U) >> 8U)
		| ((value & 0xFF000000U) >> 24U);
}

static uint64_t fw_cfg_to_be64(uint64_t value)
{
	uint64_t low = fw_cfg_to_be32((uint32_t)value);
	uint64_t high = fw_cfg_to_be32((uint32_t)(value >> 32U));
	return (low << 32U) | high;
}

static uint32_t fw_cfg_from_be32(uint32_t value)
{
	return fw_cfg_to_be32(value);
}

typedef struct {
	volatile uint32_t control;
	uint32_t length;
	uint64_t address;
} fw_cfg_dma_access_t;

static bool fw_cfg_mmio_base(
	const platform_region_t *region,
	uint64_t *base
)
{
	if (
		region == 0 ||
		base == 0 ||
		region->size < FW_CFG_MIN_SIZE ||
		!vmm_higher_half_direct_map_enabled()
	) {
		return false;
	}

	return vmm_physical_to_higher_half(region->base, base);
}

static inline void fw_cfg_barrier(void)
{
	__asm__ volatile("dmb osh" : : : "memory");
}

static inline uint8_t fw_cfg_read8(uint64_t base)
{
	return *(volatile uint8_t *)(base + FW_CFG_DATA_OFFSET);
}

static inline void fw_cfg_select(uint64_t base, uint16_t selector)
{
	*(volatile uint16_t *)(base + FW_CFG_SELECTOR_OFFSET) = fw_cfg_to_be16(selector);
	fw_cfg_barrier();
}

static inline void fw_cfg_dma_trigger(uint64_t base, uint64_t physical)
{
	*(volatile uint64_t *)(base + FW_CFG_DMA_OFFSET) = fw_cfg_to_be64(physical);
	fw_cfg_barrier();
}

static uint16_t fw_cfg_read_be16_data(uint64_t base)
{
	uint16_t value = (uint16_t)fw_cfg_read8(base) << 8U;
	value |= fw_cfg_read8(base);
	return value;
}

static uint32_t fw_cfg_read_be32_data(uint64_t base)
{
	uint32_t value = (uint32_t)fw_cfg_read8(base) << 24U;
	value |= (uint32_t)fw_cfg_read8(base) << 16U;
	value |= (uint32_t)fw_cfg_read8(base) << 8U;
	value |= fw_cfg_read8(base);
	return value;
}

static bool fw_cfg_string_equals(const char *left, const char *right)
{
	if (left == 0 || right == 0) return false;

	while (*left != '\0' && *right != '\0') {
		if (*left != *right) return false;
		left++;
		right++;
	}

	return *left == '\0' && *right == '\0';
}

static bool fw_cfg_validate(uint64_t base)
{
	fw_cfg_select(base, FW_CFG_SIGNATURE);

	if (fw_cfg_read8(base) != 'Q') return false;

	if (fw_cfg_read8(base) != 'E') return false;
	if (fw_cfg_read8(base) != 'M') return false;

	if (fw_cfg_read8(base) != 'U') return false;

	fw_cfg_select(base, FW_CFG_ID);

	uint32_t id = fw_cfg_read8(base);
	id |= (uint32_t)fw_cfg_read8(base) << 8U;
	id |= (uint32_t)fw_cfg_read8(base) << 16U;
	id |= (uint32_t)fw_cfg_read8(base) << 24U;

	return (id & FW_CFG_ID_DMA) != 0U;
}

bool fw_cfg_find_file(
	const platform_region_t *region,
	const char *name,
	fw_cfg_file_t *file
)
{
	if (name == 0 || file == 0) return false;

	uint64_t base;
	if (!fw_cfg_mmio_base(region, &base)) return false;

	if (!fw_cfg_validate(base)) return false;

	fw_cfg_select(base, FW_CFG_FILE_DIR);

	uint32_t count = fw_cfg_read_be32_data(base);
	if (count > FW_CFG_MAX_FILES) return false;

	for (uint32_t index = 0U; index < count; index++) {
		uint32_t size = fw_cfg_read_be32_data(base);
		uint16_t selector = fw_cfg_read_be16_data(base);

		(void)fw_cfg_read8(base);
		(void)fw_cfg_read8(base);

		char file_name[FW_CFG_FILE_NAME_SIZE + 1U];
		for (uint32_t offset = 0U; offset < FW_CFG_FILE_NAME_SIZE; offset++) {
			file_name[offset] = (char)fw_cfg_read8(base);
		}
		file_name[FW_CFG_FILE_NAME_SIZE] = '\0';

		if (!fw_cfg_string_equals(file_name, name)) continue;

		file->selector = selector;
		file->size = size;
		return true;
	}

	return false;
}

bool fw_cfg_dma_write(
	const platform_region_t *region,
	uint16_t selector,
	const void *data,
	uint32_t size
)
{
	if (
		data == 0 ||
		size == 0U ||
		size > PMM_PAGE_SIZE - FW_CFG_DMA_BOUNCE_OFFSET
	) {
		return false;
	}

	uint64_t base;
	if (!fw_cfg_mmio_base(region, &base)) return false;

	if (!fw_cfg_validate(base)) return false;

	uint64_t page_physical;
	if (!pmm_allocate_page(&page_physical)) return false;

	uint64_t page_virtual;
	if (!vmm_physical_to_higher_half(page_physical, &page_virtual)) {
		(void)pmm_free_page(page_physical);
		return false;
	}

	memset((void *)page_virtual, 0, PMM_PAGE_SIZE);

	fw_cfg_dma_access_t *access = (fw_cfg_dma_access_t *)page_virtual;
	void *bounce = (void *)(page_virtual + FW_CFG_DMA_BOUNCE_OFFSET);
	uint64_t bounce_physical = page_physical + FW_CFG_DMA_BOUNCE_OFFSET;

	memcpy(bounce, data, size);

	uint32_t control = ((uint32_t)selector << 16U)
		| FW_CFG_DMA_CTL_SELECT
		| FW_CFG_DMA_CTL_WRITE;

	access->control = fw_cfg_to_be32(control);
	access->length = fw_cfg_to_be32(size);
	access->address = fw_cfg_to_be64(bounce_physical);

	fw_cfg_barrier();
	fw_cfg_dma_trigger(base, page_physical);

	bool success = false;

	for (uint32_t spin = 0U; spin < 1000000U; spin++) {
		fw_cfg_barrier();

		uint32_t completed = fw_cfg_from_be32(access->control);
		if (completed == 0U) {
			success = true;
			break;
		}

		if ((completed & FW_CFG_DMA_CTL_ERROR) != 0U) break;
		__asm__ volatile("yield");
	}

	(void)pmm_free_page(page_physical);
	return success;
}
