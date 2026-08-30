#include <kern/console/console.h>
#include <platform/dtb.h>
#include <platform/uart.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE   0x00000002U
#define FDT_PROP       0x00000003U
#define FDT_NOP        0x00000004U
#define FDT_END        0x00000009U

#define DTB_HEADER_SIZE 40U
#define DTB_MAX_DEPTH 64U

static dtb_t g_boot_dtb;
static bool g_boot_dtb_ready;
static bool g_boot_dtb_higher_half;

_Static_assert(
	sizeof(dtb_header_t) == DTB_HEADER_SIZE,
	"dtb_header_t must match the DTB header layout"
);

typedef struct {
	uint32_t node_count;
	uint32_t property_count;
} dtb_dump_context_t;

static uint32_t dtb_read_be32(const void *address)
{
	const uint8_t *bytes = address;

	return ((uint32_t)bytes[0] << 24U)
		| ((uint32_t)bytes[1] << 16U)
		| ((uint32_t)bytes[2] << 8U)
		| (uint32_t)bytes[3];
}

static bool dtb_range_is_valid(
	uint32_t total_size,
	uint32_t offset,
	uint32_t size
)
{
	if (offset > total_size) {
		return false;
	}

	return size <= total_size - offset;
}

static bool dtb_has_bytes(
	const uint8_t *cursor,
	const uint8_t *end,
	uint32_t byte_count
)
{
	if (cursor > end) {
		return false;
	}

	return byte_count <= (uint32_t)(end - cursor);
}

static bool dtb_align4(uint32_t value, uint32_t *aligned)
{
	if (aligned == 0 || value > 0xFFFFFFFCU) {
		return false;
	}

	*aligned = (value + 3U) & ~3U;

	return true;
}

static bool dtb_string_length(
	const uint8_t *string,
	const uint8_t *end,
	uint32_t *length
)
{
	if (string == 0 || end == 0 || length == 0 || string >= end) {
		return false;
	}

	uint32_t result = 0;

	while (string + result < end) {
		if (string[result] == '\0') {
			*length = result;
			return true;
		}

		if (result == 0xFFFFFFFFU) {
			return false;
		}

		result++;
	}

	return false;
}

bool dtb_bootstrap(const void *address)
{
	if (g_boot_dtb_ready || address == 0) {
		return false;
	}

	memset(&g_boot_dtb, 0, sizeof(g_boot_dtb));

	if (!dtb_init(&g_boot_dtb, address)) {
		memset(&g_boot_dtb, 0, sizeof(g_boot_dtb));
		return false;
	}

	g_boot_dtb_ready = true;
	return true;
}

const dtb_t *dtb_get_boot(void)
{
	return g_boot_dtb_ready ? &g_boot_dtb : 0;
}

bool dtb_enter_higher_half(void)
{
	if (
		!g_boot_dtb_ready ||
		g_boot_dtb_higher_half ||
		!vmm_higher_half_direct_map_enabled()
	) {
		return false;
	}

	uint64_t base;
	uint64_t header;
	uint64_t reservations;
	uint64_t structure;
	uint64_t strings;

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_boot_dtb.base,
		&base
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_boot_dtb.header,
		&header
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_boot_dtb.memory_reservations,
		&reservations
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_boot_dtb.structure,
		&structure
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		(uint64_t)g_boot_dtb.strings,
		&strings
	)) {
		return false;
	}

	g_boot_dtb.base = (const void *)base;
	g_boot_dtb.header = (const dtb_header_t *)header;
	g_boot_dtb.memory_reservations = (const uint8_t *)reservations;
	g_boot_dtb.structure = (const uint8_t *)structure;
	g_boot_dtb.strings = (const char *)strings;
	g_boot_dtb_higher_half = true;

	return true;
}

bool dtb_higher_half_enabled(void)
{
	return g_boot_dtb_higher_half;
}

bool dtb_init(dtb_t *dtb, const void *address)
{
	if (dtb == 0 || address == 0) {
		return false;
	}

	const dtb_header_t *header = address;

	uint32_t magic = dtb_read_be32(&header->magic);

	if (magic != DTB_MAGIC) {
		return false;
	}

	uint32_t total_size = dtb_read_be32(&header->total_size);

	uint32_t structure_offset = dtb_read_be32(&header->structure_offset);

	uint32_t strings_offset = dtb_read_be32(&header->strings_offset);

	uint32_t memory_reservation_offset = dtb_read_be32(&header->memory_reservation_offset);

	uint32_t version = dtb_read_be32(&header->version);

	uint32_t last_compatible_version = dtb_read_be32(&header->last_compatible_version);

	uint32_t boot_cpu_id = dtb_read_be32(&header->boot_cpu_id);

	uint32_t strings_size = dtb_read_be32(&header->strings_size);

	uint32_t structure_size = dtb_read_be32(&header->structure_size);

	if (total_size < DTB_HEADER_SIZE) {
		return false;
	}

	if ((structure_offset & 0x3U) != 0U) {
		return false;
	}

	if ((memory_reservation_offset & 0x7U) != 0U) {
		return false;
	}

	if (!dtb_range_is_valid(
		total_size,
		structure_offset,
		structure_size
	)) {
		return false;
	}

	if (!dtb_range_is_valid(
		total_size,
		strings_offset,
		strings_size
	)) {
		return false;
	}

	/*
	 * The reservation block must contain at least its terminating
	 * address=0, size=0 pair.
	 */
	if (!dtb_range_is_valid(
		total_size,
		memory_reservation_offset,
		16U
	)) {
		return false;
	}

	const uint8_t *base = address;

	dtb->base = address;
	dtb->header = header;

	dtb->total_size = total_size;
	dtb->version = version;
	dtb->last_compatible_version = last_compatible_version;
	dtb->boot_cpu_id = boot_cpu_id;

	dtb->memory_reservations = base + memory_reservation_offset;

	dtb->structure = base + structure_offset;

	dtb->structure_size = structure_size;

	dtb->strings = (const char *)(base + strings_offset);

	dtb->strings_size = strings_size;

	return true;
}

bool dtb_walk(
	const dtb_t *dtb,
	const dtb_visitor_t *visitor,
	void *context
)
{
	if (dtb == 0 || dtb->structure == 0 || dtb->strings == 0) {
		return false;
	}

	const uint8_t *cursor = dtb->structure;
	const uint8_t *end = dtb->structure + dtb->structure_size;

	const uint8_t *strings_end = (const uint8_t *)dtb->strings + dtb->strings_size;

	uint32_t depth = 0;

	while (cursor < end) {
		if (!dtb_has_bytes(cursor, end, 4U)) {
			return false;
		}

		uint32_t token = dtb_read_be32(cursor);
		cursor += 4U;

		switch (token) {
		case FDT_BEGIN_NODE: {
			uint32_t name_length;

			if (!dtb_string_length(
				cursor,
				end,
				&name_length
			)) {
				return false;
			}

			uint32_t name_storage_size;

			if (!dtb_align4(
				name_length + 1U,
				&name_storage_size
			)) {
				return false;
			}

			if (!dtb_has_bytes(
				cursor,
				end,
				name_storage_size
			)) {
				return false;
			}

			if (depth >= DTB_MAX_DEPTH) {
				return false;
			}

			const char *node_name = (const char *)cursor;

			if (
				visitor != 0 &&
				visitor->begin_node != 0
			) {
				visitor->begin_node(
					node_name,
					depth,
					context
				);
			}

			cursor += name_storage_size;
			depth++;

			break;
		}

		case FDT_END_NODE:
			if (depth == 0U) {
				return false;
			}

			depth--;

			if (
				visitor != 0 &&
				visitor->end_node != 0
			) {
				visitor->end_node(
					depth,
					context
				);
			}

			break;

		case FDT_PROP: {
			/*
			 * A property header consists of:
			 *
			 * uint32_t length;
			 * uint32_t name_offset;
			 */
			if (!dtb_has_bytes(cursor, end, 8U)) {
				return false;
			}

			if (depth == 0U) {
				return false;
			}

			uint32_t length = dtb_read_be32(cursor);

			uint32_t name_offset = dtb_read_be32(cursor + 4U);

			cursor += 8U;

			if (name_offset >= dtb->strings_size) {
				return false;
			}

			const uint8_t *property_name =
				(const uint8_t *)dtb->strings +
				name_offset;

			uint32_t property_name_length;

			if (!dtb_string_length(
				property_name,
				strings_end,
				&property_name_length
			)) {
				return false;
			}

			/*
			 * The length itself is not currently needed after
			 * validation, but checking it ensures the property
			 * name really is terminated inside the strings block.
			 */
			(void)property_name_length;

			uint32_t padded_length;

			if (!dtb_align4(length, &padded_length)) {
				return false;
			}

			if (!dtb_has_bytes(
				cursor,
				end,
				padded_length
			)) {
				return false;
			}

			const void *property_value = cursor;

			if (
				visitor != 0 &&
				visitor->property != 0
			) {
				visitor->property(
					(const char *)property_name,
					property_value,
					length,
					depth - 1U,
					context
				);
			}

			cursor += padded_length;

			break;
		}

		case FDT_NOP:
			/*
			 * FDT_NOP deliberately contains no data.
			 */
			break;

		case FDT_END:
			/*
			 * Every FDT_BEGIN_NODE must have a matching
			 * FDT_END_NODE before the final FDT_END.
			 */
			if (depth != 0U) {
				return false;
			}

			/*
			 * FDT_END must be the final token in the structure
			 * block.
			 */
			return cursor == end;

		default:
			return false;
		}
	}

	/*
	 * Reaching the end without FDT_END means the structure block
	 * was truncated or malformed.
	 */
	return false;
}

static void dtb_dump_indent(uint32_t depth)
{
	for (uint32_t index = 0; index < depth; index++) {
		kputs("  ");
	}
}

static void dtb_dump_begin_node(
	const char *name,
	uint32_t depth,
	void *context
)
{
	dtb_dump_context_t *dump_context = context;

	dump_context->node_count++;

	kputs("dtb: ");
	dtb_dump_indent(depth);
	kputs("node ");

	/*
	 * The root node has an empty name in the binary DTB.
	 */
	if (depth == 0U && name[0] == '\0') {
		kputln("/");
		return;
	}

	kputln(name);
}

static void dtb_dump_property(
	const char *name,
	const void *value,
	uint32_t length,
	uint32_t depth,
	void *context
)
{
	(void)value;

	dtb_dump_context_t *dump_context = context;

	dump_context->property_count++;

	kputs("dtb: ");
	dtb_dump_indent(depth + 1U);
	kputs("property ");
	kputs(name);
	kputs(" (");
	kputu64(length);
	kputln(" bytes)");
}

bool dtb_dump(const dtb_t *dtb)
{
	dtb_dump_context_t context = {
		.node_count = 0,
		.property_count = 0
	};

	/*
	 * This runs before the permanent TTBR1 transition. Materialize callback
	 * addresses at runtime so ADRP/ADD follows the current physical alias.
	 * A constant aggregate can be folded into .rodata with higher-half
	 * absolute pointers, which are not callable while the MMU is disabled.
	 */
	dtb_visitor_t visitor;
	visitor.begin_node = dtb_dump_begin_node;
	visitor.property = dtb_dump_property;
	visitor.end_node = 0;

	if (!dtb_walk(dtb, &visitor, &context)) {
		return false;
	}

	kputs("dtb: walk complete: ");
	kputu64(context.node_count);
	kputs(" nodes, ");
	kputu64(context.property_count);
	kputln(" properties");

	return true;
}
