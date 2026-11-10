/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * sudo -- run a single command as root. The caller must be listed in
 * /etc/sudoers and authenticates with their own password (root is not asked);
 * sudo then becomes root and runs the command, relaying the terminal to it.
 *
 * Switching to root needs privilege, so sudo must be installed setuid-root
 * (chmod 4755), exactly like su.
 */
#include <nxu/syscall.h>
#include <nxu/spawn_io.h>
#include <nxu/pwdb.h>
#include <nxu/auth.h>

#include <stdint.h>

#define SUDOERS_PATH "/Volumes/mainOS/etc/sudoers"
#define CORESERVICES "/Volumes/mainOS/System/Library/CoreServices/"

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

static const char *
basename_of(const char *path)
{
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++)
		if (*p == '/') base = p + 1;
	return base;
}

static int64_t
read_line(char *buffer, uint32_t capacity)
{
	uint32_t length = 0U;
	while (length + 1U < capacity) {
		char c;
		int64_t count = nxu_read(0ULL, &c, 1ULL);
		if (count <= 0) {
			if (length == 0U) return -1;
			break;
		}
		if (c == '\n' || c == '\r') break;
		buffer[length++] = c;
	}
	buffer[length] = '\0';
	return (int64_t)length;
}

/* Is `name` listed in /etc/sudoers? One name per line; '#' starts a comment. */
static int
sudoers_allows(const char *name)
{
	int64_t fd = nxu_open(SUDOERS_PATH, NXU_O_READ);
	if (fd < 0) return 0;

	char buffer[2048];
	uint64_t total = 0ULL;
	while (total + 1ULL < sizeof(buffer)) {
		int64_t count = nxu_read((uint64_t)fd, buffer + total, sizeof(buffer) - 1ULL - total);
		if (count <= 0) break;
		total += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)fd);
	buffer[total] = '\0';

	const char *line = buffer;
	while (*line != '\0') {
		while (*line == ' ' || *line == '\t') line++;
		if (*line != '#' && *line != '\n' && *line != '\0') {
			/* Compare the first whitespace-delimited token to name. */
			uint32_t i = 0U;
			while (name[i] != '\0' && line[i] == name[i]) i++;
			if (name[i] == '\0' && (line[i] == '\0' || line[i] == '\n' || line[i] == ' ' || line[i] == '\t'))
				return 1;
		}
		while (*line != '\0' && *line != '\n') line++;
		if (*line == '\n') line++;
	}
	return 0;
}

/* Resolve a command name to a path: absolute/relative as given, else searched
 * in CoreServices. */
static void
resolve_command(const char *command, char *out, uint32_t capacity)
{
	int has_slash = 0;
	for (const char *p = command; *p != '\0'; p++)
		if (*p == '/') has_slash = 1;

	uint32_t pos = 0U;
	if (!has_slash) {
		const char *prefix = CORESERVICES;
		while (*prefix != '\0' && pos + 1U < capacity) out[pos++] = *prefix++;
	}
	while (*command != '\0' && pos + 1U < capacity) out[pos++] = *command++;
	out[pos] = '\0';
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc < 2U) {
		say("usage: sudo <command> [args...]\n");
		return 2;
	}

	nxu_passwd_t caller;
	if (nxu_getpwuid((uint32_t)nxu_getuid(), &caller) != 0) {
		say("sudo: cannot identify the calling user\n");
		return 1;
	}

	if (!sudoers_allows(caller.pw_name)) {
		say("sudo: ");
		say(caller.pw_name);
		say(" is not in the sudoers file.\n");
		return 1;
	}

	/* A non-root caller proves it is really them with their own password. */
	if (nxu_getuid() != 0) {
		say("[sudo] password for ");
		say(caller.pw_name);
		say(": ");
		char password[128];
		int64_t length = read_line(password, sizeof(password));
		say("\n");
		if (length < 0 || !nxu_authenticate(caller.pw_name, password)) {
			say("sudo: authentication failure\n");
			return 1;
		}
	}

	if (nxu_setgid(0U) < 0 || nxu_setuid(0U) < 0) {
		say("sudo: could not become root\n");
		return 1;
	}

	char path[256];
	resolve_command(argv[1], path, sizeof(path));

	const char *child_argv[NXU_EXEC_ARGV_MAX + 1U];
	child_argv[0] = basename_of(path);
	uint32_t count = 1U;
	for (uint64_t i = 2ULL; i < argc && count < NXU_EXEC_ARGV_MAX; i++) child_argv[count++] = argv[i];
	child_argv[count] = 0;

	int64_t stream = -1;
	int64_t child = nxu_spawn_io(path, child_argv, &stream);
	if (child < 0) {
		say("sudo: ");
		say(argv[1]);
		say(": command not found\n");
		return 1;
	}

	uint64_t status = 0ULL;
	int64_t result;
	do {
		char buffer[512];
		int64_t n = nxu_read_poll((uint32_t)stream, buffer, sizeof(buffer));
		if (n > 0) (void)nxu_write(1ULL, buffer, (uint64_t)n);

		n = nxu_read_poll(0U, buffer, sizeof(buffer));
		if (n > 0) (void)nxu_write((uint64_t)stream, buffer, (uint64_t)n);

		result = nxu_waitpid((uint64_t)child, &status);
		if (result == -NXU_SYS_E_AGAIN) (void)nxu_sleep_us(1000ULL);
	} while (result == -NXU_SYS_E_AGAIN);

	return (int)(status & 0xFFULL);
}
