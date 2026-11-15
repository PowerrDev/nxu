/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * su -- start a shell as another user (default root).
 *
 *     su [-l] [name]
 *
 * The target is looked up in /etc/passwd; su switches its groups, group and
 * user IDs to that account and then runs the account's shell on the caller's
 * terminal, waiting for it. Exiting the shell returns to the caller. With -l
 * (or -) the shell starts in the account's home directory, as a login would
 * (Terminal opens its tabs this way, as the computer's user).
 *
 * Switching identity needs privilege: the real root may become anyone, while a
 * non-root caller must give the target account's password (read without echo,
 * nxu_read_password). To let a non-root user run su at all, the binary must be
 * installed setuid-root (chmod 4755).
 */
#include <nxu/syscall.h>
#include <nxu/spawn_io.h>
#include <nxu/pwdb.h>
#include <nxu/auth.h>
#include <nxu/string.h>

#include <stdbool.h>
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

int
main(uint64_t argc, const char *const *argv)
{
	bool login_shell = false;
	uint64_t next = 1U;

	if (next < argc && (nxu_streq(argv[next], "-l") || nxu_streq(argv[next], "-"))) {
		login_shell = true;
		next++;
	}
	const char *target = next < argc ? argv[next] : "root";

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
		char password[128];
		int64_t length = nxu_read_password("Password:", password, sizeof(password));
		if (length < 0 || !nxu_authenticate(target, password)) {
			say("su: authentication failure\n");
			return 1;
		}
	}

	/* Groups first: once the effective UID leaves root, they can no longer be
	 * changed. The supplementary list too, or the caller's (root's wheel)
	 * would come along. */
	if (nxu_setgroups(1U, &pw.pw_gid) < 0 || nxu_setgid(pw.pw_gid) < 0 || nxu_setuid(pw.pw_uid) < 0) {
		say("su: could not switch identity\n");
		return 1;
	}

	if (login_shell && (pw.pw_dir[0] == '\0' || nxu_chdir(pw.pw_dir) < 0)) {
		say("su: no home directory, starting in /\n");
		(void)nxu_chdir("/");
	}

	const char *shell = pw.pw_shell[0] != '\0' ? pw.pw_shell : SU_DEFAULT_SHELL;
	const char *const arguments[] = {basename_of(shell), 0};

	/*
	 * The new shell runs on su's own descriptors, in this job, as if su had
	 * become it; su only waits, leaving the keyboard's signals to the shell,
	 * and ends as it does.
	 */
	(void)nxu_signal(NXU_SIGINT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGQUIT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGTSTP, (void (*)(int))NXU_SIG_IGN);

	int64_t child = nxu_spawn_here(shell, arguments);

	if (child < 0) {
		say("su: could not start shell ");
		say(shell);
		say("\n");
		return 1;
	}

	return nxu_wait_as_own(child) < 0 ? 1 : 0;
}
