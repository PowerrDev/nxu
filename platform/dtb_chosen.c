#include <platform/dtb.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	const char *property_name;
	const void *value;
	uint32_t length;
	bool in_chosen;
	bool found;
} dtb_chosen_context_t;

static void
dtb_chosen_begin_node(const char *name, uint32_t depth, void *context_pointer)
{
	dtb_chosen_context_t *context = context_pointer;
	if (context == 0 || name == 0) return;

	if (depth == 1U) context->in_chosen = strcmp(name, "chosen") == 0;
}

static void
dtb_chosen_property(const char *name, const void *value, uint32_t length, uint32_t depth, void *context_pointer)
{
	dtb_chosen_context_t *context = context_pointer;
	if (context == 0 || !context->in_chosen || depth != 1U || context->found || name == 0) return;
	if (strcmp(name, context->property_name) != 0) return;

	context->value = value;
	context->length = length;
	context->found = true;
}

static void
dtb_chosen_end_node(uint32_t depth, void *context_pointer)
{
	dtb_chosen_context_t *context = context_pointer;
	if (context == 0) return;
	if (depth == 1U) context->in_chosen = false;
}

bool
dtb_chosen_property_get(const dtb_t *dtb, const char *name, const void **value, uint32_t *length)
{
	if (value != 0) *value = 0;
	if (length != 0) *length = 0U;
	if (dtb == 0 || name == 0 || value == 0 || length == 0) return false;

	dtb_chosen_context_t context = {
		.property_name = name,
		.value = 0,
		.length = 0U,
		.in_chosen = false,
		.found = false
	};

	/*
	 * NVRAM is imported before the permanent TTBR1 transition. Build the
	 * visitor at runtime so the callback addresses use the current physical
	 * kernel alias instead of higher-half absolute pointers from .rodata.
	 */
	dtb_visitor_t visitor;
	visitor.begin_node = dtb_chosen_begin_node;
	visitor.property = dtb_chosen_property;
	visitor.end_node = dtb_chosen_end_node;

	if (!dtb_walk(dtb, &visitor, &context) || !context.found) return false;
	*value = context.value;
	*length = context.length;
	return true;
}
