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
	 * so running with effective root). Only real root may switch without a
	 * password; everyone else needs authentication, which does not exist yet.
	 */
	if (nxu_getuid() != 0) {
		say("su: password authentication is not available yet\n");
		return 1;
	}

	/* Group first: once the effective UID leaves root, the gid can no longer
	 * be changed. */
	if (nxu_setgid(pw.pw_gid) < 0 || nxu_setuid(pw.pw_uid) < 0) {
		say("su: could not switch identity\n");
		return 1;
	}

	const char *shell = pw.pw_shell[0] != '\0' ? pw.pw_shell : SU_DEFAULT_SHELL;
	const char *const arguments[] = {shell, 0};

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
