/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * getty -- keep a login prompt on a terminal: by default /dev/console, the
 * serial terminal. bootd starts it as root (com.nxu.getty) and starts it
 * again if it ends.
 *
 * getty makes itself the leader of a new session and opens the terminal,
 * which becomes the session's controlling terminal and getty's standard
 * input, output and error. Then, for ever:
 *
 *   - the terminal is put back the way a login expects it: owned by root,
 *     mode 0600, the default settings, typeahead discarded;
 *   - the banner and "login: " are shown and a name is read;
 *   - login runs on the terminal with that name (login -t <terminal> <name>)
 *     and getty waits for it, which is for the whole session: login runs the
 *     user's shell and ends when it does;
 *   - the session's end goes into utmp and wtmp (login recorded its
 *     beginning), and the terminal is revoked (nxu_revoke), so nothing the
 *     session left behind keeps a file on it, and opened again.
 *
 * NXU has no exec on i386, so getty does not become login as a BSD getty
 * does; it stays, as the session leader, and waits. It ignores the
 * keyboard's signals (^C, ^\, ^Z), which go to the foreground group it
 * shares with login: a ^C at the password prompt ends login, and getty asks
 * again.
 */
#include <nxu/spawn_io.h>
#include <nxu/syscall.h>
#include <nxu/utmp.h>
#include <termios.h>

#include <stdbool.h>
#include <stdint.h>

#define GETTY_DEFAULT_TERMINAL "/dev/console"
#define GETTY_LOGIN "/Volumes/mainOS/System/Library/CoreServices/login"
#define GETTY_HOSTNAME "/Volumes/mainOS/etc/hostname"

#define GETTY_NAME_MAX 32U

/* A terminal's mode while nobody is logged in on it: root's alone. */
#define GETTY_IDLE_MODE 0600U

static void
say(const char *text)
{
	uint64_t length = 0ULL;

	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

/* The machine's name for the banner: the first word of /etc/hostname, or none. */
static void
read_hostname(char *buffer, uint32_t capacity)
{
	buffer[0] = '\0';

	int64_t descriptor = nxu_open(GETTY_HOSTNAME, NXU_O_READ);

	if (descriptor < 0) return;

	char raw[64];
	int64_t count = nxu_read((uint64_t)descriptor, raw, sizeof(raw));
	uint32_t length = 0U;

	(void)nxu_close((uint64_t)descriptor);

	for (int64_t index = 0; index < count && length + 1U < capacity; index++) {
		char character = raw[index];

		if (character == '\n' || character == '\r' || character == ' ' || character == '\t' || character == '\0')
			break;
		buffer[length++] = character;
	}

	buffer[length] = '\0';
}

/*
 * Open `path` as the session's controlling terminal and standard input,
 * output and error, replacing whatever they were (revoked files, after a
 * session). False, with the reason on the old standard error, when it
 * cannot be done.
 */
static bool
open_terminal(const char *path)
{
	for (uint64_t descriptor = 0U; descriptor < 3U; descriptor++)
		(void)nxu_close(descriptor);

	/* A session leader with no terminal acquires the first one it opens. */
	int64_t terminal = nxu_open(path, NXU_O_READ | NXU_O_WRITE);

	if (terminal < 0) {
		say("getty: cannot open ");
		say(path);
		say("\n");
		return false;
	}

	for (uint32_t descriptor = 0U; descriptor < 3U; descriptor++) {
		if ((uint64_t)terminal != descriptor && nxu_dup2((uint32_t)terminal, descriptor) < 0) return false;
	}

	if (terminal > 2) (void)nxu_close((uint64_t)terminal);

	return isatty(0) != 0;
}

/* Root's again, as it was before anyone logged in, with the settings a login expects and no typeahead. */
static void
reset_terminal(const char *path)
{
	struct termios termios;

	(void)nxu_chown(path, 0U, 0U);
	(void)nxu_chmod(path, GETTY_IDLE_MODE);

	if (tcgetattr(0, &termios) == 0) {
		termios.c_iflag = NXU_TTYDEF_IFLAG;
		termios.c_oflag = NXU_TTYDEF_OFLAG;
		termios.c_lflag = NXU_TTYDEF_LFLAG;
		termios.c_cflag = (termios.c_cflag & ~(tcflag_t)NXU_CSIZE) | NXU_CS8 | NXU_CREAD;
		termios.c_cc[VEOF] = NXU_CEOF;
		termios.c_cc[VERASE] = NXU_CERASE;
		termios.c_cc[VWERASE] = NXU_CWERASE;
		termios.c_cc[VKILL] = NXU_CKILL;
		termios.c_cc[VINTR] = NXU_CINTR;
		termios.c_cc[VQUIT] = NXU_CQUIT;
		termios.c_cc[VSUSP] = NXU_CSUSP;
		termios.c_cc[VMIN] = 1U;
		termios.c_cc[VTIME] = 0U;
		(void)tcsetattr(0, TCSAFLUSH, &termios);
	}

	(void)tcflush(0, TCIFLUSH);
	(void)tcsetpgrp(0, (int)nxu_getpgid(0));
}

/*
 * One line from the terminal, the newline dropped. A name is printable and
 * has no blanks or colons (it is a field of /etc/passwd). False for an empty
 * line, end of file or anything that is not a name.
 */
static bool
read_name(char *name, uint32_t capacity)
{
	char line[128];
	int64_t count = nxu_read(0ULL, line, sizeof(line) - 1U);

	if (count <= 0) return false;

	uint32_t length = 0U;

	for (int64_t index = 0; index < count; index++) {
		char character = line[index];

		if (character == '\n' || character == '\r') break;
		if (character <= ' ' || character > '~' || character == ':') return false;
		if (length + 1U >= capacity) return false;
		name[length++] = character;
	}

	name[length] = '\0';
	return length != 0U;
}

int
main(uint64_t argc, const char *const *argv)
{
	const char *terminal = argc >= 2U ? argv[1] : GETTY_DEFAULT_TERMINAL;
	char host[64];

	(void)nxu_signal(NXU_SIGINT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGQUIT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGTSTP, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGHUP, (void (*)(int))NXU_SIG_IGN);

	if (nxu_setsid() < 0) {
		say("getty: cannot start a session\n");
		return 1;
	}

	if (!open_terminal(terminal)) return 1;

	/* A session utmp still shows here ended with the machine: it is over, without a logout. */
	(void)nxu_utmp_clear(terminal);

	read_hostname(host, sizeof(host));

	for (;;) {
		char name[GETTY_NAME_MAX + 1U];

		reset_terminal(terminal);

		say("\ngroveOS");
		if (host[0] != '\0') {
			say(" (");
			say(host);
			say(")");
		}
		say(" (console)\n\n");

		do {
			say("login: ");
		} while (!read_name(name, sizeof(name)));

		const char *const arguments[] = {"login", "-t", terminal, name, 0};
		int64_t child = nxu_spawn_here(GETTY_LOGIN, arguments);

		if (child < 0) {
			say("getty: cannot start login\n");
			(void)nxu_sleep_us(5000000ULL);
			continue;
		}

		uint64_t status = 0ULL;

		while (nxu_wait((uint64_t)child, &status) == -NXU_SYS_E_INTERRUPTED) {
		}

		/*
		 * The session is over: whatever it left running loses the terminal
		 * (its files read end of file and fail to write), and getty takes it
		 * up again afresh for the next one.
		 */
		/* login recorded the session if it began one; its end is recorded here, by root. */
		if (nxu_utmp_active(terminal)) (void)nxu_utmp_logout(terminal, nxu_time());

		(void)nxu_revoke(terminal);
		if (!open_terminal(terminal)) return 1;
	}
}
