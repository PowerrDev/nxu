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

#define NZH_CHILD_MAX 100U
#define NZH_FORKBOMB_MAX_SECONDS 100U

#if defined(__i386__)
static int64_t g_streams[NZH_CHILD_MAX];
#endif

static char g_directory[256] = "/Volumes/mainOS";
static volatile uint32_t g_children[NZH_CHILD_MAX];
static volatile bool g_interrupted;

static void
nzh_signal(int signal)
{
	g_interrupted = signal == NXU_SIGINT;
	for (uint32_t index = 0; index < NZH_CHILD_MAX; index++) {
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

	const char *directories[] = {"/Volumes/mainOS/System/Library/CoreServices", "/Volumes/mainOS/bin"};

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
	uint32_t slot = 0;
	while (slot < NZH_CHILD_MAX && g_children[slot] != 0) slot++;
	if (slot == NZH_CHILD_MAX) return -1;
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
		for (uint32_t index = 0; index < NZH_CHILD_MAX; index++) {
			if (g_children[index] != 0 && g_streams[index] >= 0) {
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
nzh_nvram(uint32_t count, const char *const *arguments)
{
	char value[512];
	int64_t length = nxu_get_boot_args(value, sizeof(value));
	if (length < 0) {
		netutil_write("nvram: could not read boot arguments\n");
		return;
	}
	if ((uint64_t)length >= sizeof(value)) length = (int64_t)sizeof(value) - 1;
	value[length] = '\0';

	if (count == 1 || nxu_streq(arguments[1], "boot-args")) {
		netutil_write("boot-args=");
		netutil_write(value);
		netutil_write("\n");
		return;
	}
	static const char prefix[] = "boot-args=";
	bool assignment = count == 2 && nxu_strlen(arguments[1]) > sizeof(prefix) - 1U;
	for (uint32_t index = 0U; assignment && index < sizeof(prefix) - 1U; index++) {
		if (arguments[1][index] != prefix[index]) assignment = false;
	}
	if (assignment) {
		netutil_write("nvram: this QEMU firmware exposes boot-args read-only. Set it on the next boot with make BOOT_ARGS=\"");
		netutil_write(arguments[1] + sizeof(prefix) - 1U);
		netutil_write("\". Example: make run BOOT_ARGS=\"-v\"\n");
		return;
	}
	netutil_write("usage: nvram [boot-args|boot-args=VALUE]\n");
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
nzh_parse_bounded(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value)
{
	uint32_t result = 0;
	if (text == 0 || text[0] == 0) return false;

	for (uint32_t index = 0; text[index] != 0; index++) {
		if (text[index] < '0' || text[index] > '9') return false;
		uint32_t digit = (uint32_t)(text[index] - '0');
		if (result > (maximum - digit) / 10U) return false;
		result = result * 10U + digit;
	}

	if (result < minimum || result > maximum) return false;
	*value = result;
	return true;
}

static void
nzh_number_text(uint32_t value, char text[11])
{
	char reversed[10];
	uint32_t length = 0;
	do {
		reversed[length++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && length < sizeof(reversed));
	for (uint32_t index = 0; index < length; index++) text[index] = reversed[length - index - 1];
	text[length] = 0;
}

static uint64_t
nzh_forkbomb_worker(uint32_t seconds)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + (uint64_t)seconds * 1000000ULL;
	volatile uint64_t work = 1;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		for (uint32_t index = 0; index < 10000U; index++) work = work * 1664525U + 1013904223U;
		(void)nxu_yield();
	}
	return work == 0 ? 1 : 0;
}

static void
nzh_forkbomb(uint32_t count, const char *const *arguments)
{
	uint32_t workers = 8U, seconds = 5U;
	if ((count > 1 && !nzh_parse_bounded(arguments[1], 1U, NZH_CHILD_MAX, &workers)) ||
		(count > 2 && !nzh_parse_bounded(arguments[2], 1U, NZH_FORKBOMB_MAX_SECONDS, &seconds)) || count > 3) {
		netutil_write("usage: forkbomb [workers 1-100] [seconds 1-100]\n");
		return;
	}

	char seconds_text[11];
	nzh_number_text(seconds, seconds_text);
	uint32_t started = 0;
	netutil_write("forkbomb: starting ");
	netutil_number(workers);
	netutil_write(" bounded CPU worker(s) for ");
	netutil_number(seconds);
	netutil_write(" second(s); Ctrl-C stops them\n");

	for (uint32_t slot = 0; slot < workers; slot++) {
		int64_t child;
#if defined(__i386__)
		const char *worker_arguments[] = {"nzh", "--forkbomb-worker", seconds_text, 0};
		child = nxu_spawn_io("/Volumes/mainOS/System/Library/CoreServices/nzh", worker_arguments,
						 &g_streams[slot]);
#else
		child = nxu_fork();
		if (child == 0) (void)nxu_exit(nzh_forkbomb_worker(seconds));
#endif
		if (child < 0) {
			netutil_write("forkbomb: process limit reached after ");
			netutil_number(started);
			netutil_write(" worker(s)\n");
			break;
		}
		g_children[slot] = (uint32_t)child;
		started++;
	}

	for (uint32_t slot = 0; slot < started; slot++) nzh_wait(slot);
	netutil_write("forkbomb: workers finished\n");
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
			"nzh: help commands echo pwd cd ls cat rm clear nvram forkbomb exit\n"
			"NVRAM: nvram [boot-args|boot-args=VALUE] (QEMU firmware boot-args are read-only)\n"
			"Files: rm [-f] [-d] [--] path ...; -d removes empty directories\n"
			"Stress: forkbomb [workers 1-100] [seconds 1-100] (bounded process/CPU load)\n"
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

		if (nzh_path(count > 1 ? arguments[1] : "/Volumes/mainOS", path) && nxu_stat(path, &info) == 0 &&
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
	} else if (nxu_streq(command, "forkbomb")) {
		nzh_forkbomb(count, arguments);
	} else if (nxu_streq(command, "nvram")) {
		nzh_nvram(count, arguments);
	} else if (nxu_streq(command, "commands")) {
		nzh_list("/Volumes/mainOS/System/Library/CoreServices");
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

#if defined(__i386__)
int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 3 && nxu_streq(argv[1], "--forkbomb-worker")) {
		uint32_t seconds;
		if (!nzh_parse_bounded(argv[2], 1U, NZH_FORKBOMB_MAX_SECONDS, &seconds)) return 2;
		return (int)nzh_forkbomb_worker(seconds);
	}
#else
int
main(void)
{
#endif
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
