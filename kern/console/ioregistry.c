#include <kern/console/ioregistry.h>
#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

#define IOREG_MAX_NODES 64U
#define IOREG_NAME_MAX 47U

typedef struct {
	bool used;
	ioreg_id_t parent;
	char name[IOREG_NAME_MAX + 1U];
	char class_name[IOREG_NAME_MAX + 1U];
} ioreg_node_t;

static ioreg_node_t g_ioreg_nodes[IOREG_MAX_NODES];
static uint32_t g_ioreg_count;

/* 0 doubles as "not yet created" for these: real family ids are never 0 (Root is). */
static ioreg_id_t g_ioreg_platform;
static ioreg_id_t g_ioreg_hid;
static ioreg_id_t g_ioreg_storage;
static ioreg_id_t g_ioreg_graphics;
static ioreg_id_t g_ioreg_audio;

static void ioreg_copy(char *destination, const char *source)
{
	uint32_t index = 0U;
	while (source[index] != '\0' && index < IOREG_NAME_MAX) {
		destination[index] = source[index];
		index++;
	}
	destination[index] = '\0';
}

static ioreg_id_t ioreg_add_raw(ioreg_id_t parent, const char *name, const char *class_name)
{
	if (g_ioreg_count >= IOREG_MAX_NODES) return IOREG_INVALID;

	ioreg_id_t id = g_ioreg_count;
	ioreg_node_t *node = &g_ioreg_nodes[id];
	node->used = true;
	node->parent = parent;
	ioreg_copy(node->name, name);
	ioreg_copy(node->class_name, class_name);

	g_ioreg_count++;
	return id;
}

static void ioreg_bootstrap(void)
{
	if (g_ioreg_count != 0U) return;
	(void)ioreg_add_raw(IOREG_INVALID, "Root", "IORegistryEntry");
}

ioreg_id_t ioreg_add(ioreg_id_t parent, const char *name, const char *class_name)
{
	if (name == 0 || class_name == 0) return IOREG_INVALID;
	ioreg_bootstrap();
	return ioreg_add_raw(parent, name, class_name);
}

ioreg_id_t ioreg_family_platform(void)
{
	if (g_ioreg_platform == 0U) {
		ioreg_bootstrap();
		g_ioreg_platform = ioreg_add_raw(IOREG_ROOT, "IOPlatformExpertDevice", "NXUPlatformExpert");
	}
	return g_ioreg_platform;
}

ioreg_id_t ioreg_family_hid(void)
{
	if (g_ioreg_hid == 0U) g_ioreg_hid = ioreg_add_raw(ioreg_family_platform(), "IOHIDSystem", "IOHIDSystem");
	return g_ioreg_hid;
}

ioreg_id_t ioreg_family_storage(void)
{
	if (g_ioreg_storage == 0U) g_ioreg_storage = ioreg_add_raw(ioreg_family_platform(), "IOStorageFamily", "IOStorageFamily");
	return g_ioreg_storage;
}

ioreg_id_t ioreg_family_graphics(void)
{
	if (g_ioreg_graphics == 0U) g_ioreg_graphics = ioreg_add_raw(ioreg_family_platform(), "IOGraphicsFamily", "IOGraphicsFamily");
	return g_ioreg_graphics;
}

ioreg_id_t ioreg_family_audio(void)
{
	if (g_ioreg_audio == 0U) g_ioreg_audio = ioreg_add_raw(ioreg_family_platform(), "DriverKitAudioFamily", "DriverKitAudioFamily");
	return g_ioreg_audio;
}

static void ioreg_dump_node(ioreg_id_t id, uint32_t depth)
{
	const ioreg_node_t *node = &g_ioreg_nodes[id];

	for (uint32_t level = 0U; level < depth; level++) kputs("  ");
	if (depth != 0U) kputs("+-o ");
	kputs(node->name);
	kputs("  <class ");
	kputs(node->class_name);
	kputs(", id ");
	kputhex32(0x10000100U + id);
	kputln(">");

	for (ioreg_id_t index = 0U; index < g_ioreg_count; index++) {
		if (index != id && g_ioreg_nodes[index].used && g_ioreg_nodes[index].parent == id) {
			ioreg_dump_node(index, depth + 1U);
		}
	}
}

void ioreg_dump(void)
{
	if (g_ioreg_count == 0U) return;

	kputln("IORegistry: dumping device tree");
	ioreg_dump_node(IOREG_ROOT, 0U);
}
