/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <termios.h>

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

int64_t
nxu_spawn_pty(const char *path, const char *const *arguments, const struct nxu_winsize *size, int64_t *master)
{
	char strings[NXU_EXEC_ARGV_BYTES];
	char slave_path[NXU_PTY_NAME_MAX];
	nxu_spawn_attr_t attr = {.size = sizeof(attr),
							 .caps = (uint32_t)nxu_get_caps(),
							 .flags = NXU_SPAWN_SETSID | NXU_SPAWN_SETCTTY,
							 .ctty = 0};

	if (arguments == 0 || arguments[0] == 0 || master == 0 ||
		!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) {
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)arguments[0];

	/* Not our controlling terminal: the child's session takes it. */
	int pair = posix_openpt((int)(NXU_O_READ | NXU_O_WRITE | NXU_O_NOCTTY));

	if (pair < 0) return -NXU_SYS_E_NOT_FOUND;

	if (grantpt(pair) != 0 || unlockpt(pair) != 0 || ptsname_r(pair, slave_path, sizeof(slave_path)) != 0 ||
		(size != 0 && nxu_ioctl((uint64_t)pair, NXU_TIOCSWINSZ, (void *)size) != 0)) {
		(void)nxu_close((uint64_t)pair);
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	int64_t slave = nxu_open(slave_path, NXU_O_READ | NXU_O_WRITE | NXU_O_NOCTTY);

	if (slave < 0) {
		(void)nxu_close((uint64_t)pair);
		return slave;
	}

	attr.descriptor_count = 3;
	for (uint32_t descriptor = 0; descriptor < 3; descriptor++)
		attr.descriptors[descriptor] = (nxu_spawn_fd_t){.child = descriptor, .parent = (uint32_t)slave};

	int64_t child = nxu_spawn_attr(path, &attr);

	/* Ours either way: the child holds its own. Only the child's files keep the slave open now. */
	(void)nxu_close((uint64_t)slave);

	if (child <= 0) {
		(void)nxu_close((uint64_t)pair);
		return child;
	}

	*master = pair;
	return child;
}

int64_t
nxu_spawn_here(const char *path, const char *const *arguments)
{
	char strings[NXU_EXEC_ARGV_BYTES];
	nxu_spawn_attr_t attr = {.size = sizeof(attr), .caps = (uint32_t)nxu_get_caps()};

	if (arguments == 0 || arguments[0] == 0 ||
		!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) {
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)arguments[0];
	attr.descriptor_count = 3;

	for (uint32_t descriptor = 0; descriptor < 3; descriptor++)
		attr.descriptors[descriptor] = (nxu_spawn_fd_t){.child = descriptor, .parent = descriptor};

	return nxu_spawn_attr(path, &attr);
}

int
nxu_wait_as_own(int64_t child)
{
	uint64_t status = 0;
	int64_t result;

	do {
		result = nxu_wait((uint64_t)child, &status);
	} while (result == -NXU_SYS_E_INTERRUPTED);

	if (result < 0) return -1;

	if (NXU_EXIT_WAS_KILLED(status)) {
		uint32_t signal = (uint32_t)(status & 0xFFULL);

		(void)nxu_signal(signal, (void (*)(int))NXU_SIG_DFL);
		(void)nxu_kill((uint64_t)nxu_getpid(), signal);
	}

	nxu_exit(status & 0xFFULL);
	return -1;
}
