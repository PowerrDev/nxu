#ifndef NXU_DTB_H
#define NXU_DTB_H

#include <stdbool.h>
#include <stdint.h>

#define DTB_MAGIC 0xD00DFEEDU

typedef struct {
	uint32_t magic;
	uint32_t total_size;
	uint32_t structure_offset;
	uint32_t strings_offset;
	uint32_t memory_reservation_offset;
	uint32_t version;
	uint32_t last_compatible_version;
	uint32_t boot_cpu_id;
	uint32_t strings_size;
	uint32_t structure_size;
} dtb_header_t;

typedef struct {
	const void *base;
	const dtb_header_t *header;

	uint32_t total_size;
	uint32_t version;
	uint32_t last_compatible_version;
	uint32_t boot_cpu_id;

	const uint8_t *memory_reservations;

	const uint8_t *structure;
	uint32_t structure_size;

	const char *strings;
	uint32_t strings_size;
} dtb_t;

typedef struct {
	void (*begin_node)(
		const char *name,
		uint32_t depth,
		void *context
	);

	void (*property)(
		const char *name,
		const void *value,
		uint32_t length,
		uint32_t depth,
		void *context
	);

	void (*end_node)(
		uint32_t depth,
		void *context
	);
} dtb_visitor_t;

bool dtb_init(dtb_t *dtb, const void *address);

bool dtb_walk(
	const dtb_t *dtb,
	const dtb_visitor_t *visitor,
	void *context
);

/**
 * dtb_bootstrap - Initialize the permanent boot Device Tree view.
 * @address: Address of the flattened Device Tree Blob.
 *
 * Return: true on success, otherwise false.
 */
bool dtb_bootstrap(const void *address);

/**
 * dtb_get_boot - Return the permanent boot Device Tree view.
 *
 * Return: Boot Device Tree metadata after successful initialization,
 * otherwise null.
 */
const dtb_t *dtb_get_boot(void);

bool dtb_enter_higher_half(void);
bool dtb_higher_half_enabled(void);

bool dtb_dump(const dtb_t *dtb);

/* Return one property from the top-level /chosen node. */
bool dtb_chosen_property_get(
	const dtb_t *dtb,
	const char *name,
	const void **value,
	uint32_t *length
);

#endif
