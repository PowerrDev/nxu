/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define SPAWN_IO_MAGIC 0x4e585349U

typedef struct {
	uint32_t magic;
	uint32_t count;
	char strings[NXU_EXEC_ARGV_BYTES];
} spawn_io_packet_t;

static spawn_io_packet_t g_packet;
static const char *g_arguments[NXU_EXEC_ARGV_MAX + 1];

static void
spawn_io_name(char name[64], uint32_t pid)
{
	const char *prefix = "com.nxu.spawn.";
	uint32_t length = nxu_strlen(prefix);

	memcpy(name, prefix, length);
	for (int32_t shift = 28; shift >= 0; shift -= 4) {
		name[length++] = "0123456789abcdef"[(pid >> shift) & 15];
	}
	name[length] = 0;
}

int64_t
nxu_spawn_io(const char *path, const char *const *arguments, int64_t *stream)
{
	spawn_io_packet_t packet = {.magic = SPAWN_IO_MAGIC};
	uint32_t offset = 0;

	for (; arguments[packet.count] != 0; packet.count++) {
		if (packet.count == NXU_EXEC_ARGV_MAX) {
			return -NXU_SYS_E_INVALID_ARGUMENT;
		}
		uint64_t length = nxu_strlen(arguments[packet.count]) + 1;
		if (length > sizeof(packet.strings) - offset) {
			return -NXU_SYS_E_INVALID_ARGUMENT;
		}
		memcpy(packet.strings + offset, arguments[packet.count], length);
		offset += length;
	}

	char name[64];
	spawn_io_name(name, nxu_getpid());
	int64_t listener = nxu_socket_listen(name, 1);
	if (listener < 0) {
		return listener;
	}

	int64_t child = nxu_spawn_caps(path, arguments[0], (uint32_t)nxu_get_caps());
	if (child <= 0) {
		(void)nxu_close(listener);
		return child;
	}

	int64_t connection = nxu_socket_accept(listener);
	(void)nxu_close(listener);

	if (connection < 0 || nxu_write(connection, &packet, sizeof(packet)) != sizeof(packet)) {
		if (connection >= 0) {
			(void)nxu_close(connection);
		}
		(void)nxu_kill(child, NXU_SIGTERM);
		return -NXU_SYS_E_IO;
	}

	*stream = connection;
	return child;
}

uint64_t
nxu_spawn_io_start(const char *const **arguments)
{
	*arguments = 0;
	char name[64];
	spawn_io_name(name, nxu_getppid());
	int64_t stream = nxu_socket_connect(name);

	/* Boot services and ordinary spawn calls retain their existing entry. */
	if (stream < 0) {
		return 0;
	}

	uint32_t received = 0;
	while (received < sizeof(g_packet)) {
		int64_t count = nxu_read(stream, (char *)&g_packet + received, sizeof(g_packet) - received);
		if (count <= 0) {
			(void)nxu_exit(126);
		}
		received += count;
	}

	if (g_packet.magic != SPAWN_IO_MAGIC || g_packet.count > NXU_EXEC_ARGV_MAX) {
		(void)nxu_exit(126);
	}

	uint32_t offset = 0;
	for (uint32_t index = 0; index < g_packet.count; index++) {
		g_arguments[index] = g_packet.strings + offset;
		while (offset < sizeof(g_packet.strings) && g_packet.strings[offset] != 0) {
			offset++;
		}
		if (offset == sizeof(g_packet.strings)) {
			(void)nxu_exit(126);
		}
		offset++;
	}
	g_arguments[g_packet.count] = 0;

	if (nxu_dup2(stream, 0) < 0 || nxu_dup2(stream, 1) < 0 || nxu_dup2(stream, 2) < 0) {
		(void)nxu_exit(126);
	}
	if (stream > 2) {
		(void)nxu_close(stream);
	}

	*arguments = g_arguments;
	return g_packet.count;
}
