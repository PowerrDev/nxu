/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * su -- start a shell as another user (default root). The target is looked up
 * in /etc/passwd; su switches its group and user IDs to that account and then
 * runs the account's shell, relaying the terminal to it. Exiting the shell
 * returns to the caller.
 *
 * Switching identity needs privilege: the real root may become anyone, while a
 * non-root caller must authenticate -- and password authentication is not yet
 * available, so a non-root caller is refused for now. To let a non-root user
 * run su at all, the binary must be installed setuid-root (chmod 4755).
 */
#include <nxu/syscall.h>
#include <nxu/spawn_io.h>
#include <nxu/pwdb.h>
#include <nxu/auth.h>

#include <stdint.h>

#define SU_DEFAULT_SHELL "/Volumes/mainOS/System/Library/CoreServices/nzh"

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

/* The final path component: used as argv[0] / the process name, which must be
 * short (the full path overflows the kernel's process-name buffer). */
static const char *
basename_of(const char *path)
{
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++)
		if (*p == '/') base = p + 1;
	return base;
}

/* Read one line from standard input into buffer; returns its length, or -1. */
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

int
main(uint64_t argc, const char *const *argv)
{
	const char *target = (argc >= 2U) ? argv[1] : "root";

	nxu_passwd_t pw;
	if (nxu_getpwnam(target, &pw) != 0) {
		say("su: unknown user: ");
		say(target);
		say("\n");
		return 1;
	}

	/*
	 * The real UID is who actually invoked su (even when su is setuid-root and
	 * so running with effective root). Real root may switch without a password;
	 * everyone else must authenticate as the target account.
	 */
	if (nxu_getuid() != 0) {
		say("Password: ");
		char password[128];
		int64_t length = read_line(password, sizeof(password));
		say("\n");
		if (length < 0 || !nxu_authenticate(target, password)) {
			say("su: authentication failure\n");
			return 1;
		}
	}

	/* Group first: once the effective UID leaves root, the gid can no longer
	 * be changed. */
	if (nxu_setgid(pw.pw_gid) < 0 || nxu_setuid(pw.pw_uid) < 0) {
		say("su: could not switch identity\n");
		return 1;
	}

	const char *shell = pw.pw_shell[0] != '\0' ? pw.pw_shell : SU_DEFAULT_SHELL;
	const char *const arguments[] = {basename_of(shell), 0};

	int64_t stream = -1;
	int64_t child = nxu_spawn_io(shell, arguments, &stream);
	if (child < 0) {
		say("su: could not start shell ");
		say(shell);
		say("\n");
		return 1;
	}

	/* Relay the terminal to and from the new shell until it exits. */
	uint64_t status = 0ULL;
	int64_t result;
	do {
		char buffer[512];
		int64_t count = nxu_read_poll((uint32_t)stream, buffer, sizeof(buffer));
		if (count > 0) (void)nxu_write(1ULL, buffer, (uint64_t)count);

		count = nxu_read_poll(0U, buffer, sizeof(buffer));
		if (count > 0) (void)nxu_write((uint64_t)stream, buffer, (uint64_t)count);

		result = nxu_waitpid((uint64_t)child, &status);
		if (result == -NXU_SYS_E_AGAIN) (void)nxu_sleep_us(1000ULL);
	} while (result == -NXU_SYS_E_AGAIN);

	return 0;
}
