/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

bool
nxu_spawn_pack_arguments(const char *const *arguments, char *buffer, uint32_t capacity, uint32_t *argc, uint32_t *size)
{
	uint32_t count = 0;
	uint32_t offset = 0;

	for (; arguments != 0 && arguments[count] != 0; count++) {
		if (count == NXU_EXEC_ARGV_MAX) return false;

		uint64_t length = nxu_strlen(arguments[count]) + 1;

		if (length > capacity - offset) return false;

		memcpy(buffer + offset, arguments[count], length);
		offset += (uint32_t)length;
	}

	*argc = count;
	*size = offset;
	return true;
}

int64_t
nxu_spawn_io(const char *path, const char *const *arguments, int64_t *stream)
{
	char strings[NXU_EXEC_ARGV_BYTES];
	nxu_spawn_attr_t attr = {.size = sizeof(attr), .caps = (uint32_t)nxu_get_caps()};

	if (arguments == 0 || arguments[0] == 0 || stream == 0 ||
		!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) {
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)arguments[0];

	uint32_t ends[2];
	int64_t paired = nxu_socketpair(ends);

	if (paired < 0) return paired;

	/* The child's end is its standard input, output and error; it gets nothing else of ours. */
	attr.descriptor_count = 3;
	for (uint32_t descriptor = 0; descriptor < 3; descriptor++)
		attr.descriptors[descriptor] = (nxu_spawn_fd_t){.child = descriptor, .parent = ends[1]};

	int64_t child = nxu_spawn_attr(path, &attr);

	/* Ours either way: the child, if there is one, holds its own references to that end. */
	(void)nxu_close(ends[1]);

	if (child <= 0) {
		(void)nxu_close(ends[0]);
		return child;
	}

	*stream = ends[0];
	return child;
}
