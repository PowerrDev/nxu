/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * login -- begin a session on the terminal it runs on.
 *
 *     login [-t terminal] [name]
 *
 * Asks for a name (unless given one) and that account's password, read
 * without echo, and checks it against /etc/shadow. Everyone authenticates,
 * root included: unlike su, login does not trust whoever started it, since
 * that is getty on behalf of anyone at the keyboard. After a wrong name or
 * password it waits a second and says "Login incorrect", without saying
 * which was wrong, and asks again; after LOGIN_ATTEMPTS failures it gives up.
 *
 * On success the terminal (-t, normally given by getty, which knows its
 * name) becomes the user's: owned by them, group tty, mode 0620, as Darwin
 * leaves a login terminal. Then login takes the account's groups and user
 * ID, moves to its home directory, says when the account last logged in and
 * shows /etc/motd (neither with a .hushlogin at home), runs its shell on the
 * terminal and waits for it, and ends as the shell ends. The login goes
 * into utmp and wtmp (nxu/utmp.h) on the way, while login is still root;
 * getty records the logout. getty gives the terminal back to root afterwards.
 *
 * login needs root to change identity and the terminal's owner: getty runs
 * it as root, and it is installed setuid-root (4755) so it can also be run
 * from a shell.
 */
#include <nxu/auth.h>
#include <nxu/pwdb.h>
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/utmp.h>

#include <stdbool.h>
#include <stdint.h>

#define LOGIN_ATTEMPTS 3U
#define LOGIN_FAIL_DELAY_US 1000000ULL

#define LOGIN_MOTD "/Volumes/mainOS/etc/motd"
#define LOGIN_HUSH ".hushlogin"

#define LOGIN_DEFAULT_SHELL "/Volumes/mainOS/System/Library/CoreServices/nzh"

/* The group a login terminal belongs to, and its mode: the user's, writable by the group (write, wall). */
#define LOGIN_TTY_GID 4U
#define LOGIN_TTY_MODE 0620U

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

static uint32_t
length_of(const char *text)
{
	uint32_t length = 0U;

	while (text[length] != '\0')
		length++;
	return length;
}

/*
 * "login: " until a line that is not empty, the newline dropped. False at
 * end of file (the terminal hung up) or when it cannot be read.
 */
static bool
ask_name(char *name, uint32_t capacity)
{
	char line[128];
	int64_t count;

	do {
		say("login: ");
		count = nxu_read(0ULL, line, sizeof(line) - 1U);
	} while (count == 1 && (line[0] == '\n' || line[0] == '\r'));

	if (count <= 0) return false;

	uint32_t length = 0U;

	for (int64_t index = 0; index < count; index++) {
		if (line[index] == '\n' || line[index] == '\r') break;
		if (length + 1U < capacity) name[length++] = line[index];
	}

	name[length] = '\0';
	return length != 0U;
}

/* Whether the user asked for a quiet login: a .hushlogin in their home directory, as on BSD. */
static bool
hushed(const char *home)
{
	char path[NXU_PW_PATH_MAX + sizeof(LOGIN_HUSH) + 1U];
	uint32_t length = length_of(home);
	nxu_stat_t info;

	if (length == 0U || length + sizeof(LOGIN_HUSH) + 1U > sizeof(path)) return false;

	memcpy(path, home, length);
	path[length] = '/';
	memcpy(path + length + 1U, LOGIN_HUSH, sizeof(LOGIN_HUSH));
	return nxu_stat(path, &info) == 0;
}

/* /etc/motd, as it is, if there is one. */
static void
show_motd(void)
{
	char buffer[512];
	int64_t descriptor = nxu_open(LOGIN_MOTD, NXU_O_READ);

	if (descriptor < 0) return;

	for (;;) {
		int64_t count = nxu_read((uint64_t)descriptor, buffer, sizeof(buffer));

		if (count <= 0) break;
		(void)nxu_write(1ULL, buffer, (uint64_t)count);
	}

	(void)nxu_close((uint64_t)descriptor);
}

/* Overwrite a password once it has been used. */
static void
forget(char *secret, uint32_t capacity)
{
	volatile char *bytes = secret;

	for (uint32_t index = 0U; index < capacity; index++)
		bytes[index] = '\0';
}

int
main(uint64_t argc, const char *const *argv)
{
	const char *terminal = 0;
	char name[NXU_PW_NAME_MAX];
	uint64_t index = 1U;

	name[0] = '\0';

	if (index + 1U < argc && nxu_streq(argv[index], "-t")) {
		terminal = argv[index + 1U];
		index += 2U;
	}

	if (index < argc) {
		if (length_of(argv[index]) >= sizeof(name)) {
			say("login: name too long\n");
			return 1;
		}

		memcpy(name, argv[index], length_of(argv[index]) + 1U);
		index++;
	}

	if (index != argc) {
		say("usage: login [-t terminal] [name]\n");
		return 1;
	}

	if (nxu_geteuid() != 0) {
		say("login: not installed setuid root\n");
		return 1;
	}

	/* ^C and ^\ at the prompts end login (getty asks again); ^Z must not stop it. */
	(void)nxu_signal(NXU_SIGTSTP, (void (*)(int))NXU_SIG_IGN);

	nxu_passwd_t pw;
	bool authenticated = false;

	for (uint32_t attempt = 0U; attempt < LOGIN_ATTEMPTS && !authenticated; attempt++) {
		if (name[0] == '\0' && !ask_name(name, sizeof(name))) return 1;

		char password[128];
		int64_t length = nxu_read_password("Password:", password, sizeof(password));

		/* A name that does not exist is asked for a password all the same, and fails the same way. */
		authenticated = length >= 0 && nxu_getpwnam(name, &pw) == 0 && nxu_authenticate(name, password);
		forget(password, sizeof(password));

		if (!authenticated) {
			(void)nxu_sleep_us(LOGIN_FAIL_DELAY_US);
			say("Login incorrect\n");
			name[0] = '\0';
		}
	}

	if (!authenticated) return 1;

	/* The previous login, for "Last login", before this one is recorded; then this one, while still root. */
	nxu_utmp_t previous;
	bool had_previous = nxu_utmp_last_login(pw.pw_name, &previous);

	if (terminal != 0) (void)nxu_utmp_login(terminal, pw.pw_name, (uint32_t)nxu_getpid(), nxu_time());

	if (terminal != 0) {
		(void)nxu_chown(terminal, pw.pw_uid, LOGIN_TTY_GID);
		(void)nxu_chmod(terminal, LOGIN_TTY_MODE);
	}

	/* Groups first: once the user ID leaves root, they can no longer be changed. */
	if (nxu_setgroups(1U, &pw.pw_gid) < 0 || nxu_setgid(pw.pw_gid) < 0 || nxu_setuid(pw.pw_uid) < 0) {
		say("login: could not switch identity\n");
		return 1;
	}

	/* Home, as the user now, so it is a directory they may enter; BSD's words when it is not. */
	if (pw.pw_dir[0] == '\0' || nxu_chdir(pw.pw_dir) < 0) {
		say("No home directory ");
		say(pw.pw_dir);
		say("!\nLogging in with home = \"/\".\n");
		(void)nxu_chdir("/");
	}

	if (!hushed(pw.pw_dir)) {
		if (had_previous) {
			char when[20];

			nxu_utmp_format_time(previous.ut_time, nxu_utmp_local_offset(), false, when);
			say("Last login: ");
			say(when);
			say(" on ");
			say(previous.ut_line);
			say("\n");
		}

		show_motd();
	}

	const char *shell = pw.pw_shell[0] != '\0' ? pw.pw_shell : LOGIN_DEFAULT_SHELL;
	const char *const arguments[] = {basename_of(shell), 0};

	/* The shell runs on this terminal, in this job, as if login had become it; login only waits. */
	(void)nxu_signal(NXU_SIGINT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGQUIT, (void (*)(int))NXU_SIG_IGN);

	int64_t child = nxu_spawn_here(shell, arguments);

	if (child < 0) {
		say("login: could not start shell ");
		say(shell);
		say("\n");
		return 1;
	}

	return nxu_wait_as_own(child) < 0 ? 1 : 0;
}
