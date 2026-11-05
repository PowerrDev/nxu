/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/console/ioregistry.c
 *
 * Maintains the kernel I/O registry used to publish device information.
 */
#include <kern/console/ioregistry.h>
#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

#define IOREG_MAX_NODES 512U
#define IOREG_NAME_MAX 47U

typedef struct {
	bool used;
	ioreg_id_t parent;
	char name[IOREG_NAME_MAX + 1U];
	char class_name[IOREG_NAME_MAX + 1U];
	char driver_name[IOREG_NAME_MAX + 1U];
	ioreg_state_t state;
} ioreg_node_t;

static ioreg_node_t g_ioreg_nodes[IOREG_MAX_NODES];
static uint32_t g_ioreg_count;

/* 0 doubles as "not yet created" for these: real family ids are never 0 (Root is). */
static ioreg_id_t g_ioreg_platform;
static ioreg_id_t g_ioreg_hid;
static ioreg_id_t g_ioreg_storage;
static ioreg_id_t g_ioreg_graphics;
static ioreg_id_t g_ioreg_audio;

static void
ioreg_copy(char *destination, const char *source)
{
	uint32_t index = 0U;
	while (source[index] != '\0' && index < IOREG_NAME_MAX) {
		destination[index] = source[index];
		index++;
	}
	destination[index] = '\0';
}

static ioreg_id_t
ioreg_add_raw(ioreg_id_t parent, const char *name, const char *class_name)
{
	if (g_ioreg_count >= IOREG_MAX_NODES) return IOREG_INVALID;

	ioreg_id_t id = g_ioreg_count;
	ioreg_node_t *node = &g_ioreg_nodes[id];
	node->used = true;
	node->parent = parent;
	ioreg_copy(node->name, name);
	ioreg_copy(node->class_name, class_name);
	node->state = IOREG_STATE_REGISTERED;

	g_ioreg_count++;
	return id;
}

static char
ioreg_hex_digit(uint32_t value)
{
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

static uint32_t
ioreg_append_hex(char *buffer, uint32_t offset, uint32_t value, uint32_t digits)
{
	for (uint32_t shift = digits; shift > 0U; shift--) {
		buffer[offset++] = ioreg_hex_digit((value >> ((shift - 1U) * 4U)) & 0xFU);
	}
	return offset;
}

ioreg_id_t
ioreg_add_pci_device(ioreg_id_t parent, uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor,
					 uint16_t device)
{
	char name[IOREG_NAME_MAX + 1U] = "pci";
	uint32_t offset = 3U;
	offset = ioreg_append_hex(name, offset, vendor, 4U);
	name[offset++] = ',';
	offset = ioreg_append_hex(name, offset, device, 4U);
	name[offset++] = '@';
	offset = ioreg_append_hex(name, offset, bus, 2U);
	name[offset++] = ',';
	offset = ioreg_append_hex(name, offset, slot, 2U);
	name[offset++] = '.';
	offset = ioreg_append_hex(name, offset, function, 1U);
	name[offset] = '\0';

	ioreg_id_t id = ioreg_add(parent, name, "IOPCIDevice");
	ioreg_set_driver_state(id, IOREG_STATE_DISCOVERED, 0);
	return id;
}

ioreg_id_t
ioreg_add_virtio_mmio(uint64_t base)
{
	static const char digits[] = "0123456789abcdef";
	char name[IOREG_NAME_MAX + 1U] = "VirtIOMMIO@";
	uint32_t offset = 11U;
	bool started = false;
	for (int32_t shift = 60; shift >= 0; shift -= 4) {
		uint32_t digit = (uint32_t)((base >> (uint32_t)shift) & 0xFU);
		if (digit != 0U || started || shift == 0) {
			name[offset++] = digits[digit];
			started = true;
		}
	}
	name[offset] = '\0';
	ioreg_id_t id = ioreg_add(ioreg_family_platform(), name, "VirtIODevice");
	ioreg_set_driver_state(id, IOREG_STATE_DISCOVERED, 0);
	return id;
}

void
ioreg_set_driver_state(ioreg_id_t id, ioreg_state_t state, const char *driver_name)
{
	if (id == IOREG_ROOT || id == IOREG_INVALID || id >= g_ioreg_count || !g_ioreg_nodes[id].used) return;
	g_ioreg_nodes[id].state = state;
	g_ioreg_nodes[id].driver_name[0] = '\0';
	if (driver_name != 0) ioreg_copy(g_ioreg_nodes[id].driver_name, driver_name);
}

static void
ioreg_bootstrap(void)
{
	if (g_ioreg_count != 0U) return;

	(void)ioreg_add_raw(IOREG_INVALID, "Root", "IORegistryEntry");
}

ioreg_id_t
ioreg_add(ioreg_id_t parent, const char *name, const char *class_name)
{
	if (name == 0 || class_name == 0) return IOREG_INVALID;

	ioreg_bootstrap();
	if (parent != IOREG_INVALID && (parent >= g_ioreg_count || !g_ioreg_nodes[parent].used)) return IOREG_INVALID;
	return ioreg_add_raw(parent, name, class_name);
}

ioreg_id_t
ioreg_family_platform(void)
{
	if (g_ioreg_platform == 0U) {
		ioreg_bootstrap();
		g_ioreg_platform = ioreg_add_raw(IOREG_ROOT, "IOPlatformExpertDevice", "NXUPlatformExpert");
	}
	return g_ioreg_platform;
}

ioreg_id_t
ioreg_family_hid(void)
{
	if (g_ioreg_hid == 0U) g_ioreg_hid = ioreg_add_raw(ioreg_family_platform(), "IOHIDSystem", "IOHIDSystem");
	return g_ioreg_hid;
}

ioreg_id_t
ioreg_family_storage(void)
{
	if (g_ioreg_storage == 0U)
		g_ioreg_storage = ioreg_add_raw(ioreg_family_platform(), "IOStorageFamily", "IOStorageFamily");
	return g_ioreg_storage;
}

ioreg_id_t
ioreg_family_graphics(void)
{
	if (g_ioreg_graphics == 0U)
		g_ioreg_graphics = ioreg_add_raw(ioreg_family_platform(), "IOGraphicsFamily", "IOGraphicsFamily");
	return g_ioreg_graphics;
}

ioreg_id_t
ioreg_family_audio(void)
{
	if (g_ioreg_audio == 0U)
		g_ioreg_audio = ioreg_add_raw(ioreg_family_platform(), "DriverKitAudioFamily", "DriverKitAudioFamily");
	return g_ioreg_audio;
}

static void
ioreg_dump_node(ioreg_id_t id, uint32_t depth)
{
	const ioreg_node_t *node = &g_ioreg_nodes[id];

	for (uint32_t level = 0U; level < depth; level++)
		kputs("  ");
	if (depth != 0U) kputs("+-o ");
	kputs(node->name);
	kputs("  <class ");
	kputs(node->class_name);
	kputs(", ");
	switch (node->state) {
	case IOREG_STATE_DISCOVERED: kputs("discovered"); break;
	case IOREG_STATE_MATCHED: kputs("matched"); break;
	case IOREG_STATE_ACTIVE: kputs("registered, matched, active"); break;
	case IOREG_STATE_FAILED: kputs("matched, attach failed"); break;
	case IOREG_STATE_DISABLED: kputs("matched, disabled by policy"); break;
	case IOREG_STATE_UNMATCHED: kputs("registered, unmatched"); break;
	default: kputs("registered"); break;
	}
	if (node->driver_name[0] != '\0') {
		kputs(", driver ");
		kputs(node->driver_name);
	}
	kputs(", id ");
	kputhex32(0x10000100U + id);
	kputln(">");

	for (ioreg_id_t index = 0U; index < g_ioreg_count; index++) {
		if (index != id && g_ioreg_nodes[index].used && g_ioreg_nodes[index].parent == id) {
			ioreg_dump_node(index, depth + 1U);
		}
	}
}

void
ioreg_dump(void)
{
	if (g_ioreg_count == 0U) return;

	kputln("IORegistry: dumping device tree");
	ioreg_dump_node(IOREG_ROOT, 0U);
}
