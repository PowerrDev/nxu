#include <kern/console/console.h>
#include <mach/arm64/cache.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>

#define CACHE_MAX_LEVELS 7U

#define CACHE_CLIDR_CTYPE_MASK 0x7ULL

#define CACHE_TYPE_NONE 0U
#define CACHE_TYPE_INSTRUCTION 1U
#define CACHE_TYPE_DATA 2U
#define CACHE_TYPE_SEPARATE 3U
#define CACHE_TYPE_UNIFIED 4U

#define CACHE_CCSIDR_LINE_SIZE_MASK 0x7ULL
#define CACHE_CCSIDR_ASSOCIATIVITY_MASK 0x3FFULL
#define CACHE_CCSIDR_NUM_SETS_MASK 0x7FFFULL

#define CACHE_CCSIDR_ASSOCIATIVITY_SHIFT 3U
#define CACHE_CCSIDR_NUM_SETS_SHIFT 13U

#define CACHE_MMFR2_CCIDX_SHIFT 20U
#define CACHE_MMFR2_CCIDX_MASK 0xFULL

#define CACHE_CTR_IMINLINE_MASK 0xFULL
#define CACHE_CTR_DMINLINE_SHIFT 16U
#define CACHE_CTR_DMINLINE_MASK 0xFULL

#define CACHE_SCTLR_M (1ULL << 0U)
#define CACHE_SCTLR_C (1ULL << 2U)
#define CACHE_SCTLR_I (1ULL << 12U)

typedef struct {
	uint64_t clidr;
	uint64_t ctr;

	uint32_t instruction_line_size;
	uint32_t data_line_size;

	bool initialized;
} cache_state_t;

static cache_state_t g_cache;

static uint64_t cache_read_sctlr(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, SCTLR_EL1"
		: "=r"(value)
	);

	return value;
}

static uint64_t cache_read_clidr(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, CLIDR_EL1"
		: "=r"(value)
	);

	return value;
}

static uint64_t cache_read_ctr(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, CTR_EL0"
		: "=r"(value)
	);

	return value;
}

static uint64_t cache_read_mmfr2(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, ID_AA64MMFR2_EL1"
		: "=r"(value)
	);

	return value;
}

static void cache_write_csselr(uint64_t value)
{
	__asm__ volatile(
		"msr CSSELR_EL1, %0\n"
		"isb\n"
		:
		: "r"(value)
		: "memory"
	);
}

static uint64_t cache_read_ccsidr(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, CCSIDR_EL1"
		: "=r"(value)
	);

	return value;
}

static uint32_t cache_count_leading_zeros(
	uint32_t value
)
{
	uint32_t result;

	__asm__ volatile(
		"clz %w0, %w1"
		: "=r"(result)
		: "r"(value)
	);

	return result;
}

static void cache_invalidate_set_way(
	uint64_t operand
)
{
	__asm__ volatile(
		"dc isw, %0"
		:
		: "r"(operand)
		: "memory"
	);
}

static bool cache_type_has_data(
	uint32_t type
)
{
	return
		type == CACHE_TYPE_DATA ||
		type == CACHE_TYPE_SEPARATE ||
		type == CACHE_TYPE_UNIFIED;
}

static bool cache_uses_legacy_ccsidr(void)
{
	uint64_t mmfr2 = cache_read_mmfr2();

	uint32_t ccidx =
		(uint32_t)(
			(mmfr2 >> CACHE_MMFR2_CCIDX_SHIFT) &
			CACHE_MMFR2_CCIDX_MASK
		);

	/*
	 * CCIDX == 0 means the original 32-bit CCSIDR_EL1 layout.
	 *
	 * A future lesson can add support for the extended format.
	 */
	return ccidx == 0U;
}

static bool cache_invalidate_data_all(void)
{
	if (
		(cache_read_sctlr() & CACHE_SCTLR_C) !=
		0ULL
	) {
		/*
		 * Invalidating an active data cache could discard dirty
		 * data. This initialization routine is only valid while
		 * the data cache is disabled.
		 */
		return false;
	}

	if (!cache_uses_legacy_ccsidr()) {
		return false;
	}

	uint64_t clidr = cache_read_clidr();

	/*
	 * Ensure all earlier uncached stores have completed before
	 * cache maintenance begins.
	 */
	__asm__ volatile(
		"dsb sy"
		:
		:
		: "memory"
	);

	for (
		uint32_t level = 0U;
		level < CACHE_MAX_LEVELS;
		level++
	) {
		uint32_t type =
			(uint32_t)(
				(
					clidr >>
					(level * 3U)
				) &
				CACHE_CLIDR_CTYPE_MASK
			);

		if (!cache_type_has_data(type)) {
			continue;
		}

		/*
		 * CSSELR_EL1.Level occupies bits [3:1].
		 * InD == 0 selects the data or unified cache.
		 */
		uint64_t csselr = (uint64_t)level << 1U;

		cache_write_csselr(csselr);

		uint64_t ccsidr = cache_read_ccsidr();

		/*
		 * LineSize encodes log2(words per line).
		 *
		 * Because one word is four bytes:
		 *
		 * log2(bytes per line) = LineSize + 2
		 *
		 * For set/way operands, the set field begins at
		 * LineSize + 4.
		 */
		uint32_t line_shift =
			(uint32_t)(
				ccsidr &
				CACHE_CCSIDR_LINE_SIZE_MASK
			) + 4U;

		uint32_t ways_minus_one =
			(uint32_t)(
				(
					ccsidr >>
					CACHE_CCSIDR_ASSOCIATIVITY_SHIFT
				) &
				CACHE_CCSIDR_ASSOCIATIVITY_MASK
			);

		uint32_t sets_minus_one =
			(uint32_t)(
				(
					ccsidr >>
					CACHE_CCSIDR_NUM_SETS_SHIFT
				) &
				CACHE_CCSIDR_NUM_SETS_MASK
			);

		/*
		 * The way field is placed at the top of the 32-bit
		 * set/way operand.
		 *
		 * CLZ(ways - 1) gives the required shift.
		 */
		uint32_t way_shift =
			cache_count_leading_zeros(
				ways_minus_one
			);

		uint32_t way = ways_minus_one;

		for (;;) {
			uint32_t set = sets_minus_one;

			for (;;) {
				uint64_t operand =
					((uint64_t)level << 1U) |
					((uint64_t)set << line_shift) |
					((uint64_t)way << way_shift);

				cache_invalidate_set_way(
					operand
				);

				if (set == 0U) {
					break;
				}

				set--;
			}

			if (way == 0U) {
				break;
			}

			way--;
		}
	}

	/*
	 * Restore the default selection: level 0 data/unified cache.
	 */
	cache_write_csselr(0ULL);

	/*
	 * Wait for every DC ISW operation to complete.
	 */
	__asm__ volatile(
		"dsb sy\n"
		"isb\n"
		:
		:
		: "memory"
	);

	return true;
}

static void cache_invalidate_instruction_all(void)
{
	/*
	 * Invalidate all instruction cache entries to the Point of
	 * Unification, then synchronize the pipeline.
	 */
	__asm__ volatile(
		"ic iallu\n"
		"dsb sy\n"
		"isb\n"
		:
		:
		: "memory"
	);
}

static uint32_t cache_line_size_from_encoding(
	uint32_t encoding
)
{
	/*
	 * CTR_EL0 line-size fields encode log2(words per line).
	 *
	 * One word is four bytes.
	 */
	return 4U << encoding;
}

bool cache_init(void)
{
	uint64_t sctlr = cache_read_sctlr();

	if ((sctlr & CACHE_SCTLR_M) == 0ULL) {
		/*
		 * Cacheable Normal-memory attributes must already be
		 * active through the MMU.
		 */
		return false;
	}

	bool data_enabled = (sctlr & CACHE_SCTLR_C) != 0ULL;

	bool instruction_enabled = (sctlr & CACHE_SCTLR_I) != 0ULL;

	if (
		data_enabled ||
		instruction_enabled
	) {
		/*
		 * Accept an already-complete initialization, but reject
		 * a partially enabled state.
		 */
		if (
			!data_enabled ||
			!instruction_enabled
		) {
			return false;
		}

		g_cache.clidr = cache_read_clidr();

		g_cache.ctr = cache_read_ctr();

		g_cache.instruction_line_size =
			cache_line_size_from_encoding(
				(uint32_t)(
					g_cache.ctr &
					CACHE_CTR_IMINLINE_MASK
				)
			);

		g_cache.data_line_size =
			cache_line_size_from_encoding(
				(uint32_t)(
					(
						g_cache.ctr >>
						CACHE_CTR_DMINLINE_SHIFT
					) &
					CACHE_CTR_DMINLINE_MASK
				)
			);

		g_cache.initialized = true;

		return true;
	}

	g_cache.clidr = cache_read_clidr();

	g_cache.ctr = cache_read_ctr();

	g_cache.instruction_line_size =
		cache_line_size_from_encoding(
			(uint32_t)(
				g_cache.ctr &
				CACHE_CTR_IMINLINE_MASK
			)
		);

	g_cache.data_line_size =
		cache_line_size_from_encoding(
			(uint32_t)(
				(
					g_cache.ctr >>
					CACHE_CTR_DMINLINE_SHIFT
				) &
				CACHE_CTR_DMINLINE_MASK
			)
		);

	if (!cache_invalidate_data_all()) {
		return false;
	}

	cache_invalidate_instruction_all();

	sctlr =
		cache_read_sctlr() |
		CACHE_SCTLR_C |
		CACHE_SCTLR_I;

	/*
	 * The DSB completes earlier maintenance.
	 *
	 * The ISB after writing SCTLR_EL1 ensures subsequent
	 * instructions execute using the new cache configuration.
	 */
	__asm__ volatile(
		"dsb sy\n"
		"msr SCTLR_EL1, %0\n"
		"isb\n"
		:
		: "r"(sctlr)
		: "memory"
	);

	sctlr = cache_read_sctlr();

	if (
		(sctlr & CACHE_SCTLR_C) == 0ULL ||
		(sctlr & CACHE_SCTLR_I) == 0ULL
	) {
		return false;
	}

	g_cache.initialized = true;

	return true;
}

bool cache_instruction_enabled(void)
{
	return (
		cache_read_sctlr() &
		CACHE_SCTLR_I
	) != 0ULL;
}

bool cache_data_enabled(void)
{
	return (
		cache_read_sctlr() &
		CACHE_SCTLR_C
	) != 0ULL;
}

uint32_t cache_instruction_line_size(void)
{
	if (!g_cache.initialized) {
		return 0U;
	}

	return g_cache.instruction_line_size;
}

uint32_t cache_data_line_size(void)
{
	if (!g_cache.initialized) {
		return 0U;
	}

	return g_cache.data_line_size;
}

void cache_dump(void)
{
	kputs("cache: CLIDR_EL1: ");
	kputhex64(g_cache.clidr);
	kputc('\n');

	kputs("cache: CTR_EL0: ");
	kputhex64(g_cache.ctr);
	kputc('\n');

	kputs("cache: instruction line size: ");
	kputu64(
		g_cache.instruction_line_size
	);
	kputln(" bytes");

	kputs("cache: data line size: ");
	kputu64(
		g_cache.data_line_size
	);
	kputln(" bytes");

	kputs("cache: instruction cache: ");
	kputln(
		cache_instruction_enabled()
			? "enabled"
			: "disabled"
	);

	kputs("cache: data cache: ");
	kputln(
		cache_data_enabled()
			? "enabled"
			: "disabled"
	);

	kputs("cache: SCTLR_EL1: ");
	kputhex64(cache_read_sctlr());
	kputc('\n');
}

void
cache_sync_instruction_range(uint64_t address, uint64_t size)
{
	if (size == 0ULL) return;

	uint32_t data_line = cache_data_line_size();
	uint32_t instruction_line = cache_instruction_line_size();
	if (data_line == 0U || instruction_line == 0U) return;

	uint64_t end = address + size;
	if (end < address) return;

	uint64_t data_cursor = address & ~((uint64_t)data_line - 1ULL);
	while (data_cursor < end) {
		__asm__ volatile("dc cvau, %0" : : "r"(data_cursor) : "memory");
		data_cursor += data_line;
	}

	__asm__ volatile("dsb ish" : : : "memory");

	uint64_t instruction_cursor = address & ~((uint64_t)instruction_line - 1ULL);
	while (instruction_cursor < end) {
		__asm__ volatile("ic ivau, %0" : : "r"(instruction_cursor) : "memory");
		instruction_cursor += instruction_line;
	}

	__asm__ volatile("dsb ish\nisb" : : : "memory");
}
