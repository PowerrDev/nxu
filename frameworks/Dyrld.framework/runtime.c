#include <nxu/syscall.h>
#include <nxu/string.h>
#include "elf.h"
#include <nxu/dyrld_protocol.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>
#include <stdbool.h>
#include <stdint.h>

#define SYMBOL_MAX 65536U
#define NEEDED_MAX 32U

struct image {
	nxu_dyrld_image_t mapped;
	uint64_t strings, string_size;
	uint64_t symbols, symbol_count;
	uint64_t rela, rela_size;
	uint64_t plt, plt_size;
	uint64_t init, init_array, init_size;
	uint32_t needed[NEEDED_MAX], needed_count;
	uint32_t state;
};

static struct image g_images[NXU_DYRLD_IMAGES];
static uint32_t g_count;
static const char *g_error;

static bool fail(const char *s)
{
	g_error = s;
	return false;
}

static bool range(const struct image *image, uint64_t address, uint64_t size, uint32_t flags)
{
	for (uint32_t i = 0; i < image->mapped.segment_count; i++) {
		const nxu_dyrld_segment_t *segment = &image->mapped.segments[i];

		if ((segment->flags & flags) == flags && address >= segment->address &&
			address - segment->address <= segment->size && size <= segment->size - (address - segment->address))
			return true;
	}

	return false;
}

static const char *string(const struct image *image, uint64_t offset)
{
	if (offset >= image->string_size)
		return 0;

	const char *text = (const char *)(uintptr_t)(image->strings + offset);

	for (uint64_t i = offset; i < image->string_size; i++)
		if (text[i - offset] == 0)
			return text;

	return 0;
}

static bool parse(struct image *image)
{
	if (!range(image, image->mapped.dynamic, image->mapped.dynamic_size, 4))
		return fail("invalid dynamic table");

	const dyr_dynamic_t *table = (const void *)(uintptr_t)image->mapped.dynamic;
	bool terminated = false;
	uint64_t hash = 0;

	for (uint64_t i = 0; i < image->mapped.dynamic_size / sizeof(*table); i++) {
		uint64_t value = table[i].value, pointer = image->mapped.bias + value;

		if (pointer < image->mapped.bias)
			return fail("dynamic address overflow");

		switch (table[i].tag) {
		case 0:
			terminated = true;
			break;

		case 1:
			if (image->needed_count == NEEDED_MAX || value > UINT32_MAX)
				return fail("dependency limit exceeded");

			image->needed[image->needed_count++] = value;
			break;

		case 4:
			hash = pointer;
			break;

		case 5:
			image->strings = pointer;
			break;

		case 6:
			image->symbols = pointer;
			break;

		case DYR_RELOCATION_TAG:
			image->rela = pointer;
			break;

		case DYR_RELOCATION_SIZE_TAG:
			image->rela_size = value;
			break;

		case DYR_RELOCATION_ENTRY_TAG:
			if (value != sizeof(dyr_rela_t))
				return fail("unsupported relocation entry size");

			break;

		case 10:
			image->string_size = value;
			break;

		case 11:
			if (value != sizeof(dyr_symbol_t))
				return fail("unsupported symbol entry size");

			break;

		case 12:
			image->init = pointer;
			break;

		case 2:
			image->plt_size = value;
			break;

		case 20:
			if (value != DYR_RELOCATION_TAG)
				return fail("unsupported PLT relocation format");

			break;

		case 23:
			image->plt = pointer;
			break;

		case 25:
			image->init_array = pointer;
			break;

		case 27:
			image->init_size = value;
			break;

		case 16:
#if defined(__i386__)
		case 7:
		case 8:
		case 9:
#else
		case 17:
		case 18:
		case 19:
#endif
		case 22:
		case 35:
		case 36:
		case 37:
		case 0x6ffffff0:
		case 0x6ffffffc:
		case 0x6ffffffe:
			return fail("unsupported symbol versions, symbolic binding, relocation format or text relocations");
		default:
			break;
		}

		if (terminated)
			break;
	}

	if (!terminated || !range(image, hash, 8, 4))
		return fail("missing terminated table or SysV hash");

	const uint32_t *counts = (const void *)(uintptr_t)hash;

	if (counts[0] > SYMBOL_MAX || counts[1] > SYMBOL_MAX || counts[1] == 0 ||
		!range(image, hash, ((uint64_t)counts[0] + counts[1] + 2) * 4, 4))
		return fail("invalid symbol hash");

	image->symbol_count = counts[1];

	if (!range(image, image->strings, image->string_size, 4) || image->string_size > 1024 * 1024 ||
		!range(image, image->symbols, image->symbol_count * sizeof(dyr_symbol_t), 4) ||
		image->rela_size % sizeof(dyr_rela_t) || image->plt_size % sizeof(dyr_rela_t) ||
		image->rela_size > 1024 * 1024 || image->plt_size > 1024 * 1024 ||
		(image->rela_size && !range(image, image->rela, image->rela_size, 4)) ||
		(image->plt_size && !range(image, image->plt, image->plt_size, 4)) || image->init_size % sizeof(uintptr_t) ||
		image->init_size > 65536 || (image->init_size && !range(image, image->init_array, image->init_size, 4)))
		return fail("dynamic table outside mapped segments");

	return true;
}

static bool dependency_path(const char *name, char *out)
{
	if (name == 0 || name[0] == 0 || name[0] == '/')
		return fail("invalid framework dependency");

	uint32_t n = sizeof(NXU_DYRLD_FRAMEWORKS) - 1;
	memcpy(out, NXU_DYRLD_FRAMEWORKS, n);

	const char *component = name;

	while (*component) {
		const char *end = component;

		while (*end && *end != '/')
			end++;

		if (end == component || (end - component == 1 && *component == '.') ||
			(end - component == 2 && component[0] == '.' && component[1] == '.'))
			return fail("unsafe framework path");

		component = *end ? end + 1 : end;
	}

	while (*name && n + 1 < NXU_DYRLD_PATH)
		out[n++] = *name++;

	out[n] = 0;
	return *name ? fail("framework path too long") : true;
}

static bool measured(const nxu_dyrld_image_t *image)
{
	uint32_t service;

	if (!bootstrap_client_lookup(NXU_DYRLD_SERVICE, &service))
		return fail("dyrld daemon unavailable");

	int64_t reply = nxu_ipc_port_allocate();

	if (reply <= 0) {
		nxu_ipc_port_deallocate(service);
		return fail("measurement reply port unavailable");
	}

	nxu_dyrld_measurement_t request = {.version = NXU_DYRLD_PROTOCOL_VERSION};
	memcpy(request.path, image->path, sizeof(request.path));

	bool valid = nxu_ipc_send(service, &request, sizeof(request), reply) >= 0;
	uint32_t transfer = 0, type = 0;

	/* A restarted daemon cannot reply through its old request. Bound the
	 * launch wait so losing that request does not strand the application.
	 * Concurrent cold starts may queue behind multi-megabyte asset images. */
	int64_t n = -1;
	uint64_t deadline = (uint64_t)nxu_uptime_us() + 120000000ULL;
	uint64_t delay = 1000;

	while (valid) {
		n = nxu_ipc_receive(reply, &request, sizeof(request), &transfer, &type);
		if (n != -NXU_SYS_E_AGAIN || (uint64_t)nxu_uptime_us() >= deadline)
			break;

		if (nxu_sleep_us(delay) < 0)
			break;

		if (delay < 64000)
			delay *= 2;
	}

	nxu_ipc_port_deallocate(service);
	nxu_ipc_port_deallocate(reply);
	if (transfer)
		nxu_ipc_port_deallocate(transfer);

	if (n != sizeof(request) || request.version != NXU_DYRLD_PROTOCOL_VERSION || request.status != 0 ||
		!nxu_streq(request.path, image->path))
		return fail("framework measurement failed");

	for (uint32_t i = 0; i < 32; i++)
		if (request.identity[i] != image->identity[i])
			return fail("image changed during launch");

	return true;
}

static int load(const char *path)
{
	for (uint32_t i = 0; i < g_count; i++)
		if (nxu_streq(g_images[i].mapped.path, path))
			return i;

	if (g_count == NXU_DYRLD_IMAGES) {
		fail("image limit exceeded");
		return -1;
	}

	struct image *image = &g_images[g_count];
	uint32_t n = 0;

	while (path[n] && n + 1 < sizeof(image->mapped.path)) {
		image->mapped.path[n] = path[n];
		n++;
	}

	if (path[n]) {
		fail("image path too long");
		return -1;
	}

	image->mapped.path[n] = 0;

	if (nxu_dyrld(NXU_DYRLD_LOAD, &image->mapped) < 0) {
		fail("cannot map image (check framework path and enclave ASLR)");
		return -1;
	}

	uint32_t index = g_count++;

	if (!measured(&image->mapped) || !parse(image))
		return -1;

	image->state = 1;

	for (uint32_t i = 0; i < image->needed_count; i++) {
		char dependency[NXU_DYRLD_PATH];

		if (!dependency_path(string(image, image->needed[i]), dependency) || load(dependency) < 0)
			return -1;
	}

	image->state = 2;
	return index;
}

static bool symbol_address(uint32_t requester, uint32_t index, uint64_t *address)
{
	const struct image *image = &g_images[requester];

	if (index >= image->symbol_count)
		return fail("relocation symbol outside table");

	const dyr_symbol_t *own = (const void *)(uintptr_t)image->symbols;
	const dyr_symbol_t *symbol = &own[index];
	uint32_t binding = symbol->info >> 4, kind = symbol->info & 15;

	if (kind == 6 || kind == 10)
		return fail("TLS and IFUNC unsupported");

	if (symbol->section && (binding == 0 || (symbol->other & 3) != 0)) {
		*address = symbol->section == 0xfff1 ? symbol->value : image->mapped.bias + symbol->value;
		if (symbol->section != 0xfff1 && !range(image, *address, symbol->size ? symbol->size : 1, 4))
			return fail("invalid local symbol");

		return true;
	}

	const char *name = string(image, symbol->name);

	if (name == 0)
		return fail("invalid symbol name");

	bool weak = false;
	uint64_t weak_address = 0;

	for (uint32_t i = 0; i < g_count; i++) {
		const struct image *candidate = &g_images[i];
		const dyr_symbol_t *symbols = (const void *)(uintptr_t)candidate->symbols;

		for (uint32_t j = 1; j < candidate->symbol_count; j++) {
			const dyr_symbol_t *match = &symbols[j];
			uint32_t bind = match->info >> 4, type = match->info & 15;

			if (!match->section || (bind != 1 && bind != 2) || (match->other & 3) == 1 || (match->other & 3) == 2)
				continue;

			const char *match_name = string(candidate, match->name);

			if (!match_name || !nxu_streq(name, match_name))
				continue;

			if (type == 6 || type == 10)
				return fail("TLS and IFUNC unsupported");

			uint64_t value = match->section == 0xfff1 ? match->value : candidate->mapped.bias + match->value;

			if (match->section != 0xfff1 && !range(candidate, value, match->size ? match->size : 1, 4))
				return fail("symbol outside mapped segments");

			if (bind == 1) {
				*address = value;
				return true;
			}

			weak = true;
			weak_address = value;
		}
	}

	if (weak) {
		*address = weak_address;
		return true;
	}

	if (binding == 2) {
		*address = 0;
		return true;
	}

	return fail("unresolved framework symbol");
}

static bool relocate(uint32_t index, uint64_t table, uint64_t size)
{
	const struct image *image = &g_images[index];
	const dyr_rela_t *entries = (const void *)(uintptr_t)table;

	for (uint64_t i = 0; i < size / sizeof(*entries); i++) {
		const dyr_rela_t *r = &entries[i];
		uint32_t type = r->info & ((1ULL << DYR_SYMBOL_SHIFT) - 1);
		uint64_t destination = image->mapped.bias + r->offset, value;

		if (type == 0)
			continue;

		if (destination < image->mapped.bias || (destination & (sizeof(uintptr_t) - 1)) ||
			!range(image, destination, sizeof(uintptr_t), 2))
			return fail("relocation target is not writable data");

		if (type == DYR_RELATIVE) {
			if ((r->info >> DYR_SYMBOL_SHIFT) != 0)
				return fail("invalid relative relocation");

			value = image->mapped.bias;
#if defined(__i386__)
		} else if (type == 1 || type == 6 || type == 7) {
#else
		} else if (type == 257 || type == 1025 || type == 1026) {
#endif
			if (!symbol_address(index, r->info >> DYR_SYMBOL_SHIFT, &value))
				return false;
		} else
			return fail("unsupported architecture relocation");

#if defined(__i386__)
		uint32_t addend = *(uint32_t *)(uintptr_t)destination;
		/* GLOB_DAT and JMP_SLOT bind directly; RELATIVE and R_386_32 add A. */
		if (type == 6 || type == 7)
			addend = 0;
		*(uint32_t *)(uintptr_t)destination = (uint32_t)value + addend;
#else
		*(uint64_t *)(uintptr_t)destination = value + (uint64_t)r->addend;
#endif
	}

	return true;
}

static bool executable(uint64_t address)
{
	for (uint32_t i = 0; i < g_count; i++)
		if (range(&g_images[i], address, 4, 1))
			return true;

	return false;
}

static bool initialize(uint32_t index)
{
	struct image *image = &g_images[index];

	if (image->state == 3)
		return true;

	/* Cycles initialize once, in deterministic DFS order. */
	image->state = 3;

	for (uint32_t i = 0; i < image->needed_count; i++) {
		char dependency[NXU_DYRLD_PATH];

		if (!dependency_path(string(image, image->needed[i]), dependency))
			return false;

		for (uint32_t j = 0; j < g_count; j++)
			if (nxu_streq(dependency, g_images[j].mapped.path) && !initialize(j))
				return false;
	}

	if (image->init) {
		if (!executable(image->init))
			return fail("initializer is not executable");

		((void (*)(void))(uintptr_t)image->init)();
	}

	const uintptr_t *initializers = (const void *)(uintptr_t)image->init_array;

	for (uint64_t i = 0; i < image->init_size / sizeof(*initializers); i++)
		if (initializers[i] && initializers[i] != UINTPTR_MAX) {
			if (!executable(initializers[i]))
				return fail("initializer is not executable");

			((void (*)(void))(uintptr_t)initializers[i])();
		}

	return true;
}

int main(uint64_t argc, char **argv)
{
	char path[NXU_DYRLD_PATH];

	if (nxu_dyrld(NXU_DYRLD_MAIN_PATH, path) < 0)
		return 126;

	if (load(path) < 0)
		goto failed;

	for (uint32_t i = 0; i < g_count; i++) {
		if (!relocate(i, g_images[i].rela, g_images[i].rela_size) ||
			!relocate(i, g_images[i].plt, g_images[i].plt_size))
			goto failed;
	}

	if (nxu_dyrld(NXU_DYRLD_FINALIZE, 0) < 0) {
		fail("cannot finalize image protections");
		goto failed;
	}

	if (!initialize(0))
		goto failed;

	/* Each application's _start receives the same register arguments as a
	 * static executable. It owns main(), termination and any C runtime setup. */
	((void (*)(uint64_t, char **))(uintptr_t)g_images[0].mapped.entry)(argc, argv);
	return 0;

failed:
	(void)nxu_write(2, "dyrld: ", 7);

	if (g_error)
		(void)nxu_write(2, g_error, nxu_strlen(g_error));
	(void)nxu_write(2, "\n", 1);

	return 127;
}
