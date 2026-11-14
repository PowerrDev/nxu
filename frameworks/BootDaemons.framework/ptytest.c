/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/BootDaemons.framework/ptytest.c
 *
 * ptytest: a shell on a pseudo-terminal, driven the way a person at
 * Terminal.app drives it.
 *
 * The kernel side (kern/tests/termios_process_test.c) runs this program as an
 * ordinary user. It starts nzh on a pseudo-terminal of its own
 * (nxu_spawn_pty), plays the emulator on the master, typing bytes and reading
 * what comes back, and checks:
 *
 *   1  the shell starts and prompts;
 *   2  a command typed and run, its output on the terminal;
 *   3  the shell's line editing (a backspace) and the line that results;
 *   4  ^C: the line discipline interrupts the foreground job, not the shell,
 *      and the shell prompts again;
 *   5  ^Z: the job stops, the shell reports it and prompts; `fg` continues it
 *      in the foreground, where it reads the next line typed;
 *   6  the history: Up brings back the last command;
 *   7  echo off (a password prompt): what is typed is not shown, the program
 *      still reads it;
 *   8  su with a wrong password: refused, and the password never shown;
 *   9  ^C at su's password prompt: su goes, and echo is on again;
 *  10  su with the right one: a shell of that user on the same terminal
 *      (whoami), and `exit` back to the first;
 *  11  `bg`: a stopped job continued in the background; it reads the
 *      terminal, so SIGTTIN stops it again, which the shell reports at the
 *      next prompt; `jobs` lists it and `fg` finishes it;
 *  12  Escape alone is ignored, and the key after it is not lost;
 *  13  editing in a line wider than the terminal (Left keys into it);
 *  14  `exit`: the shell leaves, the master reads end of file, and the shell's
 *      exit status is 0;
 *  15  revoke, on a second pair: a file open on the slave is cut off (end of
 *      file, writes fail, no longer a terminal), a new open of the slave
 *      works, and the console, which is root's, cannot be revoked by this
 *      user.
 *
 * The jobs it runs are this program again: "ptytest block" says it is ready
 * and reads lines until one says "bye"; "ptytest password" reads a line with
 * echo off; "ptytest echo" says whether echo is on. su is the real one,
 * setuid root, with the seed account "user" (password "user").
 *
 * The exit status is 0, or the number of the check that failed.
 */
#include <termios.h>

#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define PTYTEST_SHELL "/Volumes/mainOS/System/Library/CoreServices/nzh"
#define PTYTEST_TIMEOUT_US 20000000ULL

/* Everything the master has read, and how far the checks have looked. */
static char g_transcript[16384];
static uint32_t g_length;
static uint32_t g_seen;
static int64_t g_master = -1;

/* No C library: the one comparison this needs. */
static bool
same(const char *left, const char *right, uint64_t length)
{
	for (uint64_t index = 0; index < length; index++) {
		if (left[index] != right[index]) return false;
	}

	return true;
}

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static bool
type(const char *text)
{
	uint64_t length = nxu_strlen(text);

	return nxu_write((uint64_t)g_master, text, length) == (int64_t)length;
}

/* Where `text` occurs in the transcript at or after `from`, or -1. */
static int64_t
find(const char *text, uint32_t from)
{
	uint64_t length = nxu_strlen(text);

	for (uint32_t at = from; at + length <= g_length; at++) {
		if (same(g_transcript + at, text, length)) return at;
	}

	return -1;
}

/*
 * Read the master until `text` appears after what earlier checks consumed,
 * then consume through it. False on a timeout or end of file (*closed).
 */
static bool
expect(const char *text, bool *closed)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + PTYTEST_TIMEOUT_US;

	if (closed != 0) *closed = false;

	for (;;) {
		int64_t at = find(text, g_seen);

		if (at >= 0) {
			g_seen = (uint32_t)at + (uint32_t)nxu_strlen(text);
			return true;
		}

		if ((uint64_t)nxu_uptime_us() > deadline || g_length == sizeof(g_transcript)) return false;

		int64_t count = nxu_read_poll((uint64_t)g_master, g_transcript + g_length, sizeof(g_transcript) - g_length);

		if (count == -NXU_SYS_E_AGAIN) {
			(void)nxu_sleep_us(1000);
			continue;
		}

		if (count <= 0) {
			if (closed != 0) *closed = true;
			return false;
		}

		g_length += (uint32_t)count;
	}
}

/* Wait for the end of file the master reads once the shell's side has closed. */
static bool
expect_end(void)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + PTYTEST_TIMEOUT_US;
	char buffer[256];

	while ((uint64_t)nxu_uptime_us() < deadline) {
		int64_t count = nxu_read_poll((uint64_t)g_master, buffer, sizeof(buffer));

		if (count == 0) return true;
		if (count < 0 && count != -NXU_SYS_E_AGAIN) return false;
		if (count < 0) (void)nxu_sleep_us(1000);
	}

	return false;
}

/* "ptytest block": a job that waits at the terminal for its input. */
static int
run_block(void)
{
	char line[64];

	say("ready\n");

	for (;;) {
		int64_t count = nxu_read(0, line, sizeof(line) - 1);

		if (count <= 0) return 3;

		line[count] = 0;

		if (count >= 3 && same(line, "bye", 3)) {
			say("got bye\n");
			return 0;
		}
	}
}

/* "ptytest password": a line read with echo off, as su and passwd read one. */
static int
run_password(void)
{
	struct termios saved;
	char line[64];

	if (tcgetattr(0, &saved) != 0) return 4;

	struct termios quiet = saved;

	quiet.c_lflag &= ~(tcflag_t)ECHO;
	if (tcsetattr(0, TCSAFLUSH, &quiet) != 0) return 5;

	say("Password:");

	int64_t count = nxu_read(0, line, sizeof(line) - 1);

	(void)tcsetattr(0, TCSAFLUSH, &saved);
	say("\n");

	if (count <= 0) return 6;

	line[count] = 0;
	if (line[count - 1] == '\n') line[count - 1] = 0;

	say("got ");
	say(line);
	say("\n");
	return 0;
}

/* The transcript since the last check that passed, control characters spelt out: what the screen showed. */
static void
show_transcript(void)
{
	static const char digits[] = "0123456789abcdef";
	uint32_t from = g_seen > 512 ? g_seen - 512 : 0;

	say("ptytest: the master read:\n");

	for (uint32_t at = from; at < g_length; at++) {
		uint8_t byte = (uint8_t)g_transcript[at];
		char spelt[5] = {'\\', 'x', digits[byte >> 4], digits[byte & 15], 0};

		if (byte == '\n' || (byte >= 32 && byte < 127)) {
			char plain[2] = {(char)byte, 0};
			say(plain);
		} else {
			say(spelt);
		}
	}

	say("\n");
}

/* "ptytest echo": whether the terminal echoes. */
static int
run_echo(void)
{
	struct termios now;

	if (tcgetattr(0, &now) != 0) return 7;

	say((now.c_lflag & ECHO) != 0 ? "echo is on\n" : "echo is off\n");
	return 0;
}

static int
fail(int check, const char *what)
{
	say("ptytest: FAILED check ");
	char digits[3] = {(char)('0' + check / 10), (char)('0' + check % 10), 0};
	say(check < 10 ? digits + 1 : digits);
	say(": ");
	say(what);
	say("\n");
	show_transcript();
	return check;
}

/* Check 15: revoke, on a pair of its own, as the user who owns the slave. */
static int
check_revoke(void)
{
	char name[64];
	char buffer[16];
	int master = posix_openpt(NXU_O_READ | NXU_O_WRITE);

	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, name, sizeof(name)) != 0)
		return fail(15, "a second pair");

	int64_t old = nxu_open(name, NXU_O_READ | NXU_O_WRITE | NXU_O_NOCTTY);

	if (old < 0) return fail(15, "open the slave");
	if (nxu_revoke(name) != 0) return fail(15, "the owner could not revoke the slave");
	if (nxu_read((uint64_t)old, buffer, sizeof(buffer)) != 0) return fail(15, "a revoked file did not read end of file");
	if (nxu_write((uint64_t)old, "x", 1U) >= 0) return fail(15, "a revoked file could be written");
	if (isatty((int)old)) return fail(15, "a revoked file is still a terminal");

	int64_t fresh = nxu_open(name, NXU_O_READ | NXU_O_WRITE | NXU_O_NOCTTY);

	if (fresh < 0) return fail(15, "open the slave again after revoke");
	if (nxu_write((uint64_t)master, "hi\r", 3U) != 3) return fail(15, "type on the master");

	int64_t count = nxu_read((uint64_t)fresh, buffer, sizeof(buffer));

	if (count != 3 || buffer[0] != 'h' || buffer[1] != 'i' || buffer[2] != '\n')
		return fail(15, "the new file did not read the line typed");
	if (nxu_read((uint64_t)old, buffer, sizeof(buffer)) != 0) return fail(15, "the revoked file read the new input");

	if (nxu_revoke("/dev/console") != -NXU_SYS_E_DENIED) return fail(15, "a user revoked root's console");

	(void)nxu_close((uint64_t)old);
	(void)nxu_close((uint64_t)fresh);
	(void)nxu_close((uint64_t)master);
	return 0;
}

static int
run_checks(void)
{
	const char *arguments[] = {"nzh", 0};
	struct nxu_winsize size = {.ws_row = 24, .ws_col = 80};
	int64_t shell = nxu_spawn_pty(PTYTEST_SHELL, arguments, &size, &g_master);
	bool closed = false;

	if (shell <= 0) return fail(1, "nxu_spawn_pty");
	if (!expect("% ", 0)) return fail(1, "no prompt");

	if (!type("echo hello\r") || !expect("hello\r\n", 0) || !expect("% ", 0)) return fail(2, "echo hello");

	if (!type("echo abX\x7f" "c\r") || !expect("abc\r\n", 0) || !expect("% ", 0)) return fail(3, "a corrected line");

	if (!type("ptytest block\r") || !expect("ready", 0)) return fail(4, "start a job");
	if (!type("\x03") || !expect("% ", 0)) return fail(4, "^C did not end the job");

	if (!type("ptytest block\r") || !expect("ready", 0)) return fail(5, "start a job to stop");
	if (!type("\x1a") || !expect("suspended", 0) || !expect("% ", 0)) return fail(5, "^Z did not stop the job");
	if (!type("fg\r") || !expect("ptytest", 0)) return fail(5, "fg");
	if (!type("bye\r") || !expect("got bye", 0) || !expect("% ", 0)) return fail(5, "the continued job's input");

	if (!type("\033[A\r") || !expect("fg: no such job", 0) || !expect("% ", 0)) return fail(6, "Up did not recall fg");

	uint32_t before = g_length;

	if (!type("ptytest password\r") || !expect("Password:", 0)) return fail(7, "start the password prompt");
	if (!type("hunter2\r") || !expect("got hunter2", 0) || !expect("% ", 0)) return fail(7, "the password read");

	int64_t shown = find("hunter2", before);
	int64_t got = find("got hunter2", before);

	if (shown != got + 4) return fail(7, "the password was echoed");

	before = g_length;

	if (!type("su user\r") || !expect("Password:", 0)) return fail(8, "su did not ask for a password");
	if (!type("wrongpw\r") || !expect("su: authentication failure", 0) || !expect("% ", 0))
		return fail(8, "su took a wrong password");
	if (find("wrongpw", before) >= 0) return fail(8, "su echoed the password");

	if (!type("su user\r") || !expect("Password:", 0)) return fail(9, "su did not ask again");
	if (!type("\x03") || !expect("% ", 0)) return fail(9, "^C did not end su");
	if (!type("ptytest echo\r") || !expect("echo is on", 0) || !expect("% ", 0)) return fail(9, "echo left off");

	if (!type("su user\r") || !expect("Password:", 0)) return fail(10, "su did not ask a third time");
	if (!type("user\r") || !expect("% ", 0)) return fail(10, "no shell after su");
	if (!type("whoami\r") || !expect("user\r\n", 0) || !expect("% ", 0)) return fail(10, "whoami in su's shell");
	if (!type("exit\r") || !expect("% ", 0)) return fail(10, "exit from su's shell");
	if (!type("whoami\r") || !expect("nobody\r\n", 0) || !expect("% ", 0)) return fail(10, "back as nobody");

	if (!type("ptytest block\r") || !expect("ready", 0)) return fail(11, "start a job for bg");
	if (!type("\x1a") || !expect("suspended", 0) || !expect("% ", 0)) return fail(11, "^Z before bg");
	if (!type("bg\r") || !expect("continued", 0) || !expect("% ", 0)) return fail(11, "bg");
	(void)nxu_sleep_us(200000);
	if (!type("jobs\r") || !expect("suspended  ptytest", 0) || !expect("% ", 0))
		return fail(11, "the background reader was not stopped by SIGTTIN");
	if (!type("fg\r") || !expect("ptytest", 0)) return fail(11, "fg after bg");
	if (!type("bye\r") || !expect("got bye", 0) || !expect("% ", 0)) return fail(11, "the job after bg and fg");

	if (!type("\033")) return fail(12, "type Escape");
	(void)nxu_sleep_us(300000);
	if (!type("echo after-escape\r") || !expect("\r\nafter-escape\r\n", 0) || !expect("% ", 0))
		return fail(12, "the key after Escape was lost");

	/* 90 a's, 5 Left, a Z: the Z lands 85 in, on the second row of an 80-column terminal. */
	char wide[128];
	uint32_t length = 0;

	memcpy(wide, "echo ", 5);
	length = 5;
	for (uint32_t index = 0; index < 90; index++) wide[length++] = 'a';
	wide[length] = 0;

	char tail[64];
	uint32_t tail_length = 0;

	for (uint32_t index = 0; index < 5; index++) {
		memcpy(tail + tail_length, "\033[D", 3);
		tail_length += 3;
	}
	memcpy(tail + tail_length, "Z\r", 3);
	tail[tail_length + 3] = 0;

	char expected[128];
	uint32_t expected_length = 0;

	expected[expected_length++] = '\n';
	for (uint32_t index = 0; index < 85; index++) expected[expected_length++] = 'a';
	expected[expected_length++] = 'Z';
	for (uint32_t index = 0; index < 5; index++) expected[expected_length++] = 'a';
	expected[expected_length++] = '\r';
	expected[expected_length++] = '\n';
	expected[expected_length] = 0;

	if (!type(wide) || !type(tail) || !expect(expected, 0) || !expect("% ", 0)) return fail(13, "a wide line edited");

	if (!type("exit\r")) return fail(14, "type exit");
	if (!expect_end()) return fail(14, "no end of file after exit");

	uint64_t status = 1;

	if (nxu_wait((uint64_t)shell, &status) != (int64_t)shell || status != 0) return fail(14, "the shell's exit status");

	(void)closed;
	(void)nxu_close((uint64_t)g_master);

	int revoked = check_revoke();

	if (revoked != 0) return revoked;

	say("ptytest: a shell on a pseudo-terminal edits, runs, interrupts, stops and continues jobs\n");
	return 0;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 2 && nxu_streq(argv[1], "block")) return run_block();
	if (argc == 2 && nxu_streq(argv[1], "password")) return run_password();
	if (argc == 2 && nxu_streq(argv[1], "echo")) return run_echo();

	return run_checks();
}
