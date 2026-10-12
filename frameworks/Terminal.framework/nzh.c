/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * NXU's small interactive shell. Terminal supplies canonical input, editing,
 * and history; this process parses commands and owns foreground children.
 */
#include "shell.h"
#include "netutil.h"

#if defined(__i386__)
#include <nxu/spawn_io.h>
#endif

#include <nxu/string.h>
#include <nxu/syscall.h>

#if defined(__i386__)
static int64_t g_streams[2] = {-1, -1};
#endif

static char g_directory[256] = "/disk";
static volatile uint32_t g_children[2];
static volatile bool g_interrupted;

static void
nzh_signal(int signal)
{
	g_interrupted = signal == NXU_SIGINT;
	for (uint32_t index = 0; index < 2; index++) {
		if (g_children[index] != 0) {
			(void)nxu_kill(g_children[index], signal);
		}
	}

	if (signal == NXU_SIGTERM) {
		(void)nxu_exit(0);
	}
}

static bool
nzh_join(char *output, uint32_t capacity, const char *directory, const char *name)
{
	uint64_t prefix = nxu_strlen(directory);
	uint64_t suffix = nxu_strlen(name);

	if (prefix + suffix + 2 > capacity) {
		return false;
	}

	memcpy(output, directory, prefix);
	output[prefix++] = '/';
	memcpy(output + prefix, name, suffix + 1);
	return true;
}

static bool
nzh_path(const char *name, char path[256])
{
	if (name[0] == '/') {
		if (nxu_strlen(name) >= 256) {
			return false;
		}

		memcpy(path, name, nxu_strlen(name) + 1);
		return true;
	}

	return nzh_join(path, 256, g_directory, name);
}

static bool
nzh_executable(const char *name, char path[256])
{
	for (const char *cursor = name; *cursor != 0; cursor++) {
		if (*cursor == '/') {
			return nzh_path(name, path);
		}
	}

	const char *directories[] = {"/disk/System/Library/CoreServices", "/disk/bin"};

	for (uint32_t index = 0; index < 2; index++) {
		nxu_stat_t info;

		if (nzh_join(path, 256, directories[index], name) && nxu_stat(path, &info) == 0 &&
			info.type == NXU_STAT_TYPE_REGULAR) {
			return true;
		}
	}

	return false;
}

static int64_t
nzh_launch(const char *name, const char *const *arguments)
{
	char path[256];

	if (!nzh_executable(name, path)) {
		netutil_write("nzh: command not found: ");
		netutil_write(name);
		netutil_write("\n");
		return -1;
	}

#if defined(__i386__)
	uint32_t slot = g_children[0] == 0 ? 0 : 1;
	int64_t child = nxu_spawn_io(path, arguments, &g_streams[slot]);
#else
	int64_t child = nxu_fork();

	if (child == 0) {
		(void)nxu_exec(path, arguments);
		netutil_write("nzh: could not execute ");
		netutil_write(name);
		netutil_write("\n");
		(void)nxu_exit(127);
	}

#endif

	if (child < 0) {
		netutil_write("nzh: could not create child process\n");
	}

	return child;
}

static void
nzh_wait(uint32_t slot)
{
	uint64_t status = 0;
	int64_t result;

#if defined(__i386__)
	/* Drain both children: paired diagnostics may produce output concurrently. */
	do {
		char buffer[512];
		for (uint32_t index = 0; index < 2; index++) {
			if (g_streams[index] >= 0) {
				int64_t count = nxu_read_poll(g_streams[index], buffer, sizeof(buffer));
				if (count > 0) {
					(void)nxu_write(1, buffer, count);
				}
			}
		}
		int64_t count = nxu_read_poll(0, buffer, sizeof(buffer));
		if (count > 0 && g_streams[slot] >= 0) {
			if (buffer[0] == 3) {
				nzh_signal(NXU_SIGINT);
			} else {
				(void)nxu_write(g_streams[slot], buffer, count);
			}
		}
		result = nxu_waitpid(g_children[slot], &status);
		if (result == -NXU_SYS_E_AGAIN) {
			(void)nxu_sleep_us(1000);
		}
	} while (result == -NXU_SYS_E_AGAIN);

	/* Deliver the tail of stdout after the child exits. */
	char buffer[512];
	int64_t count;
	while ((count = nxu_read_poll(g_streams[slot], buffer, sizeof(buffer))) > 0) {
		(void)nxu_write(1, buffer, count);
	}
	(void)nxu_close(g_streams[slot]);
	g_streams[slot] = -1;
#else
	do {
		result = nxu_wait(g_children[slot], &status);
	} while (result == -NXU_SYS_E_INTERRUPTED);

#endif

	g_children[slot] = 0;
	g_interrupted = false;

	if (result < 0 || status != 0) {
		netutil_write("nzh: command exited with status ");
		netutil_number(status);
		netutil_write("\n");
	}
}

static void
nzh_list(const char *name)
{
	char path[256];

	if (!nzh_path(name, path)) {
		return;
	}

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) {
		netutil_write("ls: cannot open directory\n");
		return;
	}

	nxu_dirent_t entry;

	while (nxu_readdir(descriptor, &entry) > 0) {
		netutil_write(entry.name);
		netutil_write("\n");
	}

	(void)nxu_close(descriptor);
}

static void
nzh_cat(const char *name)
{
	char path[256];

	if (!nzh_path(name, path)) {
		return;
	}

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) {
		netutil_write("cat: cannot open file\n");
		return;
	}

	char buffer[512];
	int64_t count;

	while ((count = nxu_read(descriptor, buffer, sizeof(buffer))) > 0) {
		(void)nxu_write(1, buffer, count);
	}

	(void)nxu_close(descriptor);
}

static void
nzh_remove(uint32_t count, const char *const *arguments)
{
	bool force = false;
	bool directories = false;
	uint32_t first = 1;

	for (; first < count; first++) {
		const char *option = arguments[first];

		if (nxu_streq(option, "--")) {
			first++;
			break;
		}

		if (option[0] != '-' || option[1] == 0) {
			break;
		}

		for (uint32_t index = 1; option[index] != 0; index++) {
			if (option[index] == 'f') {
				force = true;
			} else if (option[index] == 'd') {
				directories = true;
			} else {
				netutil_write("usage: rm [-f] [-d] [--] path ...\n");
				return;
			}
		}
	}

	if (first == count && !force) {
		netutil_write("usage: rm [-f] [-d] [--] path ...\n");
		return;
	}

	for (uint32_t index = first; index < count; index++) {
		char path[256];
		int64_t result = -NXU_SYS_E_INVALID_ARGUMENT;

		if (nzh_path(arguments[index], path)) {
			result = nxu_unlink(path);
			if (directories && result == -NXU_SYS_E_IS_DIRECTORY) {
				result = nxu_rmdir(path);
			}
		}

		if (result == 0 || (force && result == -NXU_SYS_E_NOT_FOUND)) {
			continue;
		}

		netutil_write("rm: ");
		netutil_write(arguments[index]);
		if (result == -NXU_SYS_E_NOT_FOUND) {
			netutil_write(": no such file or directory\n");
		} else if (result == -NXU_SYS_E_IS_DIRECTORY) {
			netutil_write(": is a directory (use -d for an empty directory)\n");
		} else if (result == -NXU_SYS_E_NOT_EMPTY) {
			netutil_write(": directory is not empty\n");
		} else if (result == -NXU_SYS_E_DENIED) {
			netutil_write(": permission denied\n");
		} else if (result == -NXU_SYS_E_READ_ONLY) {
			netutil_write(": read-only filesystem\n");
		} else {
			netutil_write(": cannot remove (error ");
			netutil_number((uint64_t)-result);
			netutil_write(")\n");
		}
	}
}

static bool
nzh_execute(uint32_t count, const char *const *arguments)
{
	const char *command = arguments[0];

	if (nxu_streq(command, "exit")) {
		return false;
	}

	if (nxu_streq(command, "help")) {
		netutil_write(
			"nzh: help commands echo pwd cd ls cat rm clear exit\n"
			"Files: rm [-f] [-d] [--] path ...; -d removes empty directories\n"
			"Network: ping [-4|-6] [-c count] address; ifconfig\n"
			"Tests: ipctest, sockettest, terminaltest, threadtest, proctest, smptest, and installed executables\n"
			"Terminal: arrows edit/history, Tab completes commands, Ctrl-C interrupts\n"
			"Quoted arguments and escapes supported; pipelines and scripting are not implemented.\n");
	} else if (nxu_streq(command, "echo")) {
		for (uint32_t index = 1; index < count; index++) {
			if (index != 1) {
				netutil_write(" ");
			}
			netutil_write(arguments[index]);
		}
		netutil_write("\n");
	} else if (nxu_streq(command, "pwd")) {
		netutil_write(g_directory);
		netutil_write("\n");
	} else if (nxu_streq(command, "cd")) {
		char path[256];
		nxu_stat_t info;

		if (nzh_path(count > 1 ? arguments[1] : "/disk", path) && nxu_stat(path, &info) == 0 &&
			info.type == NXU_STAT_TYPE_DIRECTORY) {
			memcpy(g_directory, path, nxu_strlen(path) + 1);
		} else {
			netutil_write("cd: directory not found\n");
		}
	} else if (nxu_streq(command, "ls")) {
		nzh_list(count > 1 ? arguments[1] : ".");
	} else if (nxu_streq(command, "cat")) {
		if (count == 2) {
			nzh_cat(arguments[1]);
		} else {
			netutil_write("usage: cat path\n");
		}
	} else if (nxu_streq(command, "rm")) {
		nzh_remove(count, arguments);
	} else if (nxu_streq(command, "clear")) {
		netutil_write("\033[2J\033[H");
	} else if (nxu_streq(command, "commands")) {
		nzh_list("/disk/System/Library/CoreServices");
	} else if (nxu_streq(command, "ipctest") || nxu_streq(command, "sockettest")) {
		const char *server = nxu_streq(command, "ipctest") ? "ipctest_a" : "sockettest_server";
		const char *client = nxu_streq(command, "ipctest") ? "ipctest_b" : "sockettest_client";
		const char *server_args[] = {server, 0};
		const char *client_args[] = {client, 0};
		int64_t first = nzh_launch(server, server_args);

		if (first > 0) {
			g_children[0] = first;
			int64_t second = nzh_launch(client, client_args);
			if (second > 0) {
				g_children[1] = second;
				nzh_wait(1);
			} else {
				(void)nxu_kill(first, NXU_SIGTERM);
			}
			nzh_wait(0);
		}
	} else {
		int64_t child = nzh_launch(command, arguments);

		if (child > 0) {
			g_children[0] = child;
			nzh_wait(0);
		}
	}

	return true;
}

int
main(void)
{
	(void)nxu_signal(NXU_SIGINT, nzh_signal);
	(void)nxu_signal(NXU_SIGTERM, nzh_signal);
	netutil_write("NXU shell (nzh) - type help\n");

	for (;;) {
		netutil_write("nxu% ");
		char line[NZH_LINE_MAX];
		uint32_t length = 0;
		bool overflow = false;
		bool interrupted = false;

		for (;;) {
			char character;
			int64_t result = nxu_read_poll(0, &character, 1);

			if (g_interrupted) {
				g_interrupted = false;
				interrupted = true;
				break;
			}

			if (result == -NXU_SYS_E_AGAIN) {
				(void)nxu_sleep_us(1000);
				continue;
			}

			if (result <= 0) {
				return 0;
			}

			if (character == 3) {
				interrupted = true;
				break;
			}

			if (character == '\n') {
				break;
			}

			if (length + 1 < sizeof(line)) {
				line[length++] = character;
			} else {
				overflow = true;
			}
		}

		if (interrupted) {
			continue;
		}

		line[length] = 0;
		const char *arguments[NZH_ARGUMENT_MAX + 1];
		int count = overflow ? -1 : nzh_parse(line, arguments);

		if (count < 0) {
			netutil_write("nzh: invalid quoting, unsupported operator, or command too long\n");
		} else if (count > 0 && !nzh_execute(count, arguments)) {
			return 0;
		}
	}
}
