/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/fdt.c
 *
 * Reads the firmware's flattened Device Tree and writes the copy NBL hands
 * to the arm64 kernel. The format is Devicetree Specification 0.4, chapter 5:
 * a big-endian header, a memory reservation block, a structure block of
 * tokens and a strings block of property names.
 */

#include "fdt.h"
#include "nbl.h"

#define FDT_MAGIC 0xD00DFEEDU
#define FDT_HEADER_SIZE 40U

#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_NOP 4U
#define FDT_END 9U

static guid fdt_guid = {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};

static uint32_t
be32(const void *pointer)
{
	const uint8_t *b = pointer;

	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint64_t
be64(const void *pointer)
{
	return ((uint64_t)be32(pointer) << 32) | be32((const uint8_t *)pointer + 4);
}

static void
put32(void *pointer, uint32_t value)
{
	uint8_t *b = pointer;

	b[0] = (uint8_t)(value >> 24);
	b[1] = (uint8_t)(value >> 16);
	b[2] = (uint8_t)(value >> 8);
	b[3] = (uint8_t)value;
}

static void
put64(void *pointer, uint64_t value)
{
	put32(pointer, (uint32_t)(value >> 32));
	put32((uint8_t *)pointer + 4, (uint32_t)value);
}

/* The blocks of a tree whose header has been checked against its own total size. */
typedef struct {
	const uint8_t *base;
	uint32_t total;
	const uint8_t *structure;
	uint32_t structure_size;
	const char *strings;
	uint32_t strings_size;
	const uint8_t *reservations;
	uint32_t reservations_room;
} fdt_view;

static bool
fdt_open(const void *fdt, fdt_view *view)
{
	const uint8_t *base = fdt;

	if (!base || be32(base) != FDT_MAGIC) {
		return false;
	}

	uint32_t total = be32(base + 4);
	uint32_t structure = be32(base + 8);
	uint32_t strings = be32(base + 12);
	uint32_t reservations = be32(base + 16);
	uint32_t strings_size = be32(base + 32);
	uint32_t structure_size = be32(base + 36);

	if (total < FDT_HEADER_SIZE || be32(base + 20) < 16U || structure > total || structure_size > total - structure ||
		strings > total || strings_size > total - strings || reservations > total || total - reservations < 16U ||
		(structure & 3U) || (reservations & 7U)) {
		return false;
	}

	*view = (fdt_view){base,		   total,		 base + structure,	 structure_size,
					   (const char *)base + strings, strings_size, base + reservations, total - reservations};
	return true;
}

const void *
nbl_fdt_firmware(void)
{
	fdt_view view;
	const void *fdt = nbl_config_table(&fdt_guid);

	return fdt_open(fdt, &view) ? fdt : 0;
}

uint32_t
nbl_fdt_size(const void *fdt)
{
	fdt_view view;

	return fdt_open(fdt, &view) ? view.total : 0;
}

/* One token of the structure block, with what follows it. */
typedef struct {
	uint32_t token;
	const char *name;	 /* BEGIN_NODE: the unit name. PROP: the property name. */
	const uint8_t *value; /* PROP */
	uint32_t length;	 /* PROP */
	uint32_t size;		 /* bytes the token and its payload take in the block */
} fdt_token;

/*
 * Routine:     fdt_next
 * Purpose:
 *              Decode the token at `offset`. False for a token that runs past
 *              the block or names a string outside the strings block.
 */
static bool
fdt_next(const fdt_view *view, uint32_t offset, fdt_token *out)
{
	if (offset > view->structure_size || view->structure_size - offset < 4U) {
		return false;
	}

	const uint8_t *cursor = view->structure + offset;
	uint32_t room = view->structure_size - offset - 4U;

	nbl_zero(out, sizeof(*out));
	out->token = be32(cursor);
	out->size = 4U;

	if (out->token == FDT_BEGIN_NODE) {
		uint32_t length = 0;

		while (length < room && cursor[4U + length]) {
			length++;
		}

		if (length == room) {
			return false;
		}

		out->name = (const char *)cursor + 4;
		out->size += (length + 1U + 3U) & ~3U;
		return out->size - 4U <= room;
	}

	if (out->token == FDT_PROP) {
		if (room < 8U) {
			return false;
		}

		uint32_t length = be32(cursor + 4);
		uint32_t name = be32(cursor + 8);

		if (length > room - 8U || ((length + 3U) & ~3U) > room - 8U || name >= view->strings_size) {
			return false;
		}

		/* The name must end inside the strings block. */
		uint32_t end = name;

		while (end < view->strings_size && view->strings[end]) {
			end++;
		}

		if (end == view->strings_size) {
			return false;
		}

		out->name = view->strings + name;
		out->value = cursor + 12;
		out->length = length;
		out->size += 8U + ((length + 3U) & ~3U);
		return true;
	}

	return out->token == FDT_END_NODE || out->token == FDT_NOP || out->token == FDT_END;
}

static bool
name_equal(const char *a, const char *b)
{
	uintn length = nbl_length(a);

	return length == nbl_length(b) && nbl_equal(a, b, length);
}

/* Whether a `compatible` value, a list of NUL-terminated strings, holds this one. */
static bool
compatible_holds(const uint8_t *list, uint32_t length, const char *wanted)
{
	uintn wanted_length = nbl_length(wanted);

	for (uint32_t start = 0; start < length;) {
		uint32_t end = start;

		while (end < length && list[end]) {
			end++;
		}

		if (end - start == wanted_length && nbl_equal(list + start, wanted, wanted_length)) {
			return true;
		}

		start = end + 1U;
	}

	return false;
}

bool
nbl_fdt_compatible_reg(const void *fdt, const char *compatible, uint32_t index, uint64_t *address)
{
	fdt_view view;

	if (!fdt_open(fdt, &view)) {
		return false;
	}

	/*
	 * A node's properties come before its children, so the node being read
	 * is complete at the next node boundary.
	 */
	bool matches = false;
	const uint8_t *reg = 0;
	fdt_token token;

	for (uint32_t offset = 0; fdt_next(&view, offset, &token); offset += token.size) {
		if (token.token == FDT_PROP) {
			if (name_equal(token.name, "compatible")) {
				matches = compatible_holds(token.value, token.length, compatible);
			} else if (name_equal(token.name, "reg") && token.length >= 8U) {
				reg = token.value;
			}

			continue;
		}

		if (token.token == FDT_NOP) {
			continue;
		}

		if (matches && reg) {
			if (index == 0) {
				*address = be64(reg);
				return true;
			}

			index--;
		}

		matches = false;
		reg = 0;

		if (token.token == FDT_END) {
			break;
		}
	}

	return false;
}

/* A bounded output block: writes past the end are dropped and remembered. */
typedef struct {
	uint8_t *base;
	uint32_t capacity;
	uint32_t used;
	bool overflow;
} fdt_writer;

static void
emit(fdt_writer *writer, const void *bytes, uint32_t size)
{
	if (writer->overflow || size > writer->capacity - writer->used) {
		writer->overflow = true;
		return;
	}

	nbl_copy(writer->base + writer->used, bytes, size);
	writer->used += size;
}

static void
emit32(fdt_writer *writer, uint32_t value)
{
	uint8_t bytes[4];

	put32(bytes, value);
	emit(writer, bytes, 4);
}

static void
emit_padding(fdt_writer *writer, uint32_t alignment)
{
	static const uint8_t zeros[8];

	emit(writer, zeros, (alignment - (writer->used & (alignment - 1U))) & (alignment - 1U));
}

/* The bootargs property: its value is the text and its terminator. */
static void
emit_bootargs(fdt_writer *writer, const char *bootargs, uint32_t name_offset)
{
	uint32_t length = (uint32_t)nbl_length(bootargs) + 1U;

	emit32(writer, FDT_PROP);
	emit32(writer, length);
	emit32(writer, name_offset);
	emit(writer, bootargs, length);
	emit_padding(writer, 4);
}

uint32_t
nbl_fdt_rebuild(const void *fdt, void *out, uint32_t capacity, const char *bootargs, uint64_t reserve_base,
				uint64_t reserve_size)
{
	fdt_view view;

	if (!fdt_open(fdt, &view) || capacity < FDT_HEADER_SIZE) {
		return 0;
	}

	fdt_writer writer = {out, capacity, FDT_HEADER_SIZE, false};

	nbl_zero(out, FDT_HEADER_SIZE);

	/* The memory reservation block: the firmware's entries, then the loader's, then the terminator. */
	uint32_t reservations = writer.used;

	for (uint32_t offset = 0; view.reservations_room - offset >= 16U; offset += 16U) {
		const uint8_t *entry = view.reservations + offset;

		if (!be64(entry) && !be64(entry + 8)) {
			break;
		}

		emit(&writer, entry, 16);
	}

	uint8_t entry[16];

	if (reserve_size) {
		put64(entry, reserve_base);
		put64(entry + 8, reserve_size);
		emit(&writer, entry, 16);
	}

	nbl_zero(entry, sizeof(entry));
	emit(&writer, entry, 16);

	/* The new name goes after the firmware's strings, which keep their offsets. */
	uint32_t bootargs_name = view.strings_size;
	uint32_t structure = writer.used;
	uint32_t depth = 0;
	bool in_chosen = false, written = false, ended = false;
	fdt_token token;

	for (uint32_t offset = 0; !ended && fdt_next(&view, offset, &token); offset += token.size) {
		const uint8_t *raw = view.structure + offset;

		switch (token.token) {
		case FDT_BEGIN_NODE:
			emit(&writer, raw, token.size);
			depth++;
			if (depth == 2 && name_equal(token.name, "chosen")) {
				in_chosen = true;
				emit_bootargs(&writer, bootargs, bootargs_name);
				written = true;
			}
			break;

		case FDT_END_NODE:
			if (!depth) {
				return 0;
			}

			/* A tree with no /chosen gets one as the last child of the root. */
			if (depth == 1 && !written) {
				static const uint8_t chosen[8] = "chosen";

				emit32(&writer, FDT_BEGIN_NODE);
				emit(&writer, chosen, sizeof(chosen));
				emit_bootargs(&writer, bootargs, bootargs_name);
				emit32(&writer, FDT_END_NODE);
				written = true;
			}

			if (depth == 2) {
				in_chosen = false;
			}

			depth--;
			emit(&writer, raw, token.size);
			break;

		case FDT_PROP:
			/* The firmware's own bootargs give way to the loader's. */
			if (!(in_chosen && depth == 2 && name_equal(token.name, "bootargs"))) {
				emit(&writer, raw, token.size);
			}
			break;

		case FDT_END:
			emit(&writer, raw, token.size);
			ended = true;
			break;

		default:
			break;
		}
	}

	if (!ended || depth || !written) {
		return 0;
	}

	uint32_t structure_size = writer.used - structure;
	uint32_t strings = writer.used;

	emit(&writer, view.strings, view.strings_size);
	emit(&writer, "bootargs", 9);

	uint32_t strings_size = writer.used - strings;

	emit_padding(&writer, 8);
	if (writer.overflow) {
		return 0;
	}

	uint8_t *header = out;

	put32(header, FDT_MAGIC);
	put32(header + 4, writer.used);
	put32(header + 8, structure);
	put32(header + 12, strings);
	put32(header + 16, reservations);
	put32(header + 20, 17);
	put32(header + 24, 16);
	put32(header + 28, be32(view.base + 28));
	put32(header + 32, strings_size);
	put32(header + 36, structure_size);
	return writer.used;
}
