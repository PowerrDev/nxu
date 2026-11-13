/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/BootDaemons.framework/ttytest.c
 *
 * ttytest: <termios.h> and stty, from user space, on a real terminal.
 *
 * The kernel side (kern/tests/termios_process_test.c) puts a loopback
 * terminal at /dev/ttytest, on which whatever is written comes back as
 * input, and runs this program as an ordinary user. Everything here goes
 * through system calls: the library functions, the ioctls under them, and
 * stty run as a child whose output is read back through a socket pair.
 *
 * Covered: isatty on a terminal, a file, a closed descriptor; the settings
 * a terminal starts with; tcsetattr round trips, TCSASOFT, TCSAFLUSH; raw
 * and canonical bytes through the loop (one line per read, output
 * processing on the way out, CR to NL on the way in); tcflush, tcdrain,
 * tcflow (output stopped and restarted, the stop character sent); the
 * window size; refusals; and stty: -g saved and restored, raw and -echo,
 * sane, rows and columns, control characters, -a and the short listing,
 * an unknown operand, standard input that is not a terminal.
 *
 * The exit status is 0, or the number of the check that failed.
 */
#include <termios.h>

#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define TTYTEST_DEVICE "/dev/ttytest"
#define TTYTEST_SELF "/Volumes/mainOS/System/Library/CoreServices/ttytest"
#define TTYTEST_STTY "/Volumes/mainOS/System/Library/CoreServices/stty"

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static int
fail(int check, const char *what)
{
	char digits[4] = {(char)('0' + (check / 10) % 10), (char)('0' + (check % 10)), ' ', '\0'};

	say("ttytest: FAILED check ");
	say(digits);
	say(what);
	say("\n");
	return check;
}

/* memcmp == 0; the user library has no memcmp. */
static bool
bytes_equal(const void *left, const void *right, uint64_t length)
{
	const uint8_t *a = left;
	const uint8_t *b = right;

	for (uint64_t index = 0; index < length; index++) {
		if (a[index] != b[index]) return false;
	}

	return true;
}

static bool
same(const struct termios *left, const struct termios *right)
{
	return bytes_equal(left, right, sizeof(*left));
}

static bool
contains(const char *haystack, const char *needle)
{
	uint64_t length = nxu_strlen(needle);
	uint64_t total = nxu_strlen(haystack);

	for (uint64_t start = 0; start + length <= total; start++) {
		if (bytes_equal(haystack + start, needle, length)) return true;
	}

	return false;
}

static uint32_t
queued(int descriptor, uint32_t command)
{
	uint32_t count = UINT32_MAX;

	(void)nxu_ioctl((uint64_t)descriptor, command, &count);
	return count;
}

/* What a terminal starts with: <sys/ttydefaults.h>. */
static void
initial(struct termios *t)
{
	memset(t, 0, sizeof(*t));
	t->c_iflag = NXU_TTYDEF_IFLAG;
	t->c_oflag = NXU_TTYDEF_OFLAG;
	t->c_cflag = NXU_TTYDEF_CFLAG;
	t->c_lflag = NXU_TTYDEF_LFLAG;
}

/*
 * Run stty with `arguments` (after argv[0]); its standard output comes back
 * in `output`, its standard input is the terminal unless `no_stdin`. Returns
 * the exit status, or -1 if it could not be run.
 */
static int64_t
run_stty(const char *const *arguments, bool no_stdin, int terminal, char *output, uint32_t capacity)
{
	const char *argv[12] = {"stty"};
	uint32_t count = 1U;

	for (; arguments[count - 1U] != 0 && count < 11U; count++)
		argv[count] = arguments[count - 1U];

	argv[count] = 0;

	char strings[512];
	uint32_t ends[2];
	nxu_spawn_attr_t attr = {.size = sizeof(attr)};

	if (!nxu_spawn_pack_arguments(argv, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) return -1;
	if (nxu_socketpair(ends) != 0) return -1;

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)"stty";
	attr.descriptors[attr.descriptor_count++] = (nxu_spawn_fd_t){.child = 1U, .parent = ends[1]};
	attr.descriptors[attr.descriptor_count++] = (nxu_spawn_fd_t){.child = 2U, .parent = ends[1]};

	if (!no_stdin) attr.descriptors[attr.descriptor_count++] = (nxu_spawn_fd_t){.child = 0U, .parent = (uint32_t)terminal};

	int64_t pid = nxu_spawn_attr(TTYTEST_STTY, &attr);

	(void)nxu_close(ends[1]);

	if (pid <= 0) {
		(void)nxu_close(ends[0]);
		return -1;
	}

	/* Read until the child's end closes, which it does when stty exits. */
	uint32_t length = 0U;

	for (;;) {
		int64_t got = nxu_read(ends[0], output + length, capacity - 1U - length);

		if (got <= 0) break;

		length += (uint32_t)got;
		if (length == capacity - 1U) break;
	}

	output[length] = '\0';
	(void)nxu_close(ends[0]);

	uint64_t status = 0U;

	if (nxu_wait((uint64_t)pid, &status) != pid) return -1;

	return (int64_t)status;
}

int
main(void)
{
	struct termios t;
	struct termios back;
	char buffer[64];
	char output[2048];

	int fd = (int)nxu_open(TTYTEST_DEVICE, NXU_O_READ | NXU_O_WRITE);
	int nb = (int)nxu_open(TTYTEST_DEVICE, NXU_O_READ | NXU_O_WRITE | NXU_O_NONBLOCK);
	int file = (int)nxu_open(TTYTEST_SELF, NXU_O_READ);

	if (fd < 0 || nb < 0) return fail(1, "cannot open " TTYTEST_DEVICE);
	if (file < 0) return fail(2, "cannot open a regular file");

	/* isatty. */
	if (isatty(fd) != 1 || isatty(nb) != 1) return fail(3, "a terminal is not a tty");
	if (isatty(file) != 0) return fail(4, "a regular file is a tty");
	if (isatty(-1) != 0 || isatty(40) != 0) return fail(5, "a bad descriptor is a tty");
	if (nxu_ioctl((uint64_t)file, NXU_TIOCGETA, &t) != -NXU_SYS_E_NOT_TTY) return fail(6, "a file did not say NOT_TTY");
	if (tcgetattr(file, &t) != -1) return fail(7, "tcgetattr on a file");

	/* What a terminal starts with. */
	if (tcgetattr(fd, &t) != 0) return fail(10, "tcgetattr");

	struct termios expected;

	initial(&expected);
	if (t.c_iflag != expected.c_iflag || t.c_oflag != expected.c_oflag || t.c_cflag != expected.c_cflag ||
		t.c_lflag != expected.c_lflag)
		return fail(11, "the initial modes are not the defaults");
	if (t.c_cc[VINTR] != NXU_CTRL('c') || t.c_cc[VERASE] != 0x7F || t.c_cc[VEOF] != NXU_CTRL('d') ||
		t.c_cc[VMIN] != 1 || t.c_cc[VTIME] != 0 || t.c_cc[VSTATUS] != _POSIX_VDISABLE)
		return fail(12, "the initial control characters are not the defaults");
	if (cfgetispeed(&t) != B9600 || cfgetospeed(&t) != B9600) return fail(13, "the initial speed is not 9600");

	struct termios cooked = t;

	/* Raw, and a round trip of every field. */
	struct termios raw = t;

	cfmakeraw(&raw);
	if (tcsetattr(fd, TCSANOW, &raw) != 0 || tcgetattr(nb, &back) != 0 || !same(&back, &raw))
		return fail(20, "tcsetattr TCSANOW round trip");

	/* TCSASOFT leaves the line alone. */
	struct termios soft = raw;

	(void)cfsetspeed(&soft, B1200);
	soft.c_cflag |= CLOCAL;
	soft.c_lflag |= ECHONL;
	if (tcsetattr(fd, TCSANOW | TCSASOFT, &soft) != 0 || tcgetattr(fd, &back) != 0)
		return fail(21, "tcsetattr TCSASOFT");
	if (cfgetospeed(&back) != B9600 || (back.c_cflag & CLOCAL) != 0 || (back.c_lflag & ECHONL) == 0)
		return fail(22, "TCSASOFT changed the line, or not the rest");
	if (tcsetattr(fd, TCSANOW, &raw) != 0) return fail(23, "back to raw");
	if (tcsetattr(fd, 7, &raw) != -1) return fail(24, "tcsetattr took an unknown action");

	/* Raw bytes through the loop. */
	if (nxu_write((uint64_t)fd, "abc", 3) != 3) return fail(30, "write");
	if (nxu_read((uint64_t)nb, buffer, sizeof(buffer)) != 3 || !bytes_equal(buffer, "abc", 3))
		return fail(31, "raw bytes did not come back");
	if (nxu_read((uint64_t)nb, buffer, sizeof(buffer)) != -NXU_SYS_E_AGAIN) return fail(32, "an empty terminal read");

	/* Canonical, without echo (the loop would feed it back): NL out as CR-NL, CR in as NL, one line a read. */
	struct termios quiet = cooked;

	quiet.c_lflag &= ~ECHO;
	if (tcsetattr(fd, TCSANOW, &quiet) != 0) return fail(40, "canonical mode");
	if (nxu_write((uint64_t)fd, "line\n", 5) != 5) return fail(41, "write a line");
	if (nxu_read((uint64_t)fd, buffer, sizeof(buffer)) != 5 || !bytes_equal(buffer, "line\n", 5))
		return fail(42, "a canonical read did not return exactly the line");
	if (nxu_read((uint64_t)fd, buffer, sizeof(buffer)) != 1 || buffer[0] != '\n')
		return fail(43, "the CR of CR-NL did not arrive as a line of its own");

	/* tcflush and TCSAFLUSH. */
	if (tcsetattr(fd, TCSANOW, &raw) != 0) return fail(50, "raw again");
	if (nxu_write((uint64_t)fd, "xyz", 3) != 3 || queued(fd, NXU_FIONREAD) != 3U) return fail(51, "input before flush");
	if (tcflush(fd, TCIFLUSH) != 0 || queued(fd, NXU_FIONREAD) != 0U) return fail(52, "tcflush TCIFLUSH");
	if (tcflush(fd, TCIOFLUSH) != 0 || tcflush(fd, 9) != -1) return fail(53, "tcflush TCIOFLUSH, or a bad queue");
	if (nxu_write((uint64_t)fd, "q", 1) != 1 || tcsetattr(fd, TCSAFLUSH, &raw) != 0 || queued(fd, NXU_FIONREAD) != 0U)
		return fail(54, "TCSAFLUSH kept unread input");
	if (tcdrain(fd) != 0) return fail(55, "tcdrain");

	/* tcflow: output held back and let go; the stop character sent down the line. */
	if (tcflow(fd, TCOOFF) != 0) return fail(60, "tcflow TCOOFF");
	if (nxu_write((uint64_t)nb, "s", 1) != 1 || queued(fd, NXU_TIOCOUTQ) != 1U ||
		nxu_read((uint64_t)nb, buffer, sizeof(buffer)) != -NXU_SYS_E_AGAIN)
		return fail(61, "output went out after TCOOFF");
	if (tcflow(fd, TCOON) != 0 || nxu_read((uint64_t)nb, buffer, sizeof(buffer)) != 1 || buffer[0] != 's')
		return fail(62, "tcflow TCOON");
	if (tcflow(fd, TCIOFF) != 0 || nxu_read((uint64_t)nb, buffer, sizeof(buffer)) != 1 || buffer[0] != NXU_CTRL('s'))
		return fail(63, "tcflow TCIOFF did not send VSTOP");
	if (tcflow(fd, 99) != -1) return fail(64, "tcflow took an unknown action");

	/* Window size. */
	struct winsize size = {.ws_row = 33, .ws_col = 99};
	struct winsize got;

	if (tcsetwinsize(fd, &size) != 0 || tcgetwinsize(nb, &got) != 0 || got.ws_row != 33 || got.ws_col != 99)
		return fail(70, "window size round trip");

	/* stty -g, and restoring from what it printed. */
	if (tcsetattr(fd, TCSANOW, &cooked) != 0) return fail(80, "back to the defaults");

	const char *save[] = {"-f", TTYTEST_DEVICE, "-g", 0};

	if (run_stty(save, false, fd, output, sizeof(output)) != 0) return fail(81, "stty -g");
	if (!contains(output, "gfmt1:cflag=4b00:iflag=6b02:lflag=5cb:oflag=3:") || !contains(output, ":intr=3:") ||
		!contains(output, ":ispeed=9600:ospeed=9600\n"))
		return fail(82, "stty -g printed something else");

	char saved[512];
	uint64_t saved_length = nxu_strlen(output);

	if (saved_length == 0 || saved_length >= sizeof(saved)) return fail(83, "stty -g output size");
	memcpy(saved, output, saved_length - 1);
	saved[saved_length - 1] = '\0';

	if (tcsetattr(fd, TCSANOW, &raw) != 0) return fail(84, "raw before restoring");

	const char *restore[] = {"-f", TTYTEST_DEVICE, saved, 0};

	if (run_stty(restore, false, fd, output, sizeof(output)) != 0 || tcgetattr(fd, &back) != 0 || !same(&back, &cooked))
		return fail(85, "stty did not restore what -g printed");

	/* Modes and combinations, on standard input. */
	const char *raw_noecho[] = {"raw", "-echo", 0};

	if (run_stty(raw_noecho, false, fd, output, sizeof(output)) != 0 || tcgetattr(fd, &back) != 0)
		return fail(90, "stty raw -echo");
	if ((back.c_lflag & (ICANON | ECHO | ISIG)) != 0 || (back.c_oflag & OPOST) != 0 || back.c_cc[VMIN] != 1)
		return fail(91, "stty raw -echo did not make the terminal raw");

	const char *sane[] = {"sane", "rows", "50", "cols", "132", 0};

	if (run_stty(sane, false, fd, output, sizeof(output)) != 0 || tcgetattr(fd, &back) != 0 ||
		tcgetwinsize(fd, &got) != 0)
		return fail(92, "stty sane rows cols");
	if (back.c_lflag != NXU_TTYDEF_LFLAG || back.c_iflag != NXU_TTYDEF_IFLAG || got.ws_row != 50 || got.ws_col != 132)
		return fail(93, "stty sane or the size did not take");

	/* Listings. */
	const char *none[] = {0};

	if (run_stty(none, false, fd, output, sizeof(output)) != 0) return fail(100, "stty");
	if (!nxu_streq(output, "speed 9600 baud;\n")) return fail(101, "stty listed a default setting");

	const char *list_all[] = {"-a", 0};

	if (run_stty(list_all, false, fd, output, sizeof(output)) != 0) return fail(102, "stty -a");
	if (!contains(output, "speed 9600 baud; 50 rows; 132 columns;\n") ||
		!contains(output, "lflags: icanon isig iexten echo echoe -echok echoke -echonl echoctl\n\t-echoprt") ||
		!contains(output, "oflags: opost onlcr") || !contains(output, "cflags: cs8 cread") ||
		!contains(output, "cchars: discard = ^O; dsusp = <undef>; eof = ^D;"))
		return fail(103, "stty -a printed something else");

	const char *chars[] = {"intr", "^X", "erase", "^H", "min", "3", "-icanon", 0};

	if (run_stty(chars, false, fd, output, sizeof(output)) != 0 || tcgetattr(fd, &back) != 0)
		return fail(104, "stty intr erase min");
	if (back.c_cc[VINTR] != NXU_CTRL('x') || back.c_cc[VERASE] != 0x08 || back.c_cc[VMIN] != 3 ||
		(back.c_lflag & ICANON) != 0)
		return fail(105, "stty control characters did not take");
	if (run_stty(none, false, fd, output, sizeof(output)) != 0 ||
		!nxu_streq(output, "speed 9600 baud;\nlflags: -icanon\ncchars: erase = ^H; intr = ^X; min = 3;\n"))
		return fail(106, "the short listing did not show exactly what changed");

	/* Refusals. */
	const char *bogus[] = {"frobnicate", 0};

	if (run_stty(bogus, false, fd, output, sizeof(output)) != 1 || !contains(output, "unknown argument: frobnicate"))
		return fail(110, "stty took an unknown operand");
	if (run_stty(none, true, fd, output, sizeof(output)) != 1 || !contains(output, "stdin isn't a terminal"))
		return fail(111, "stty on a stdin that is not a terminal");

	(void)nxu_close((uint64_t)file);
	(void)nxu_close((uint64_t)nb);
	(void)nxu_close((uint64_t)fd);

	say("ttytest: termios calls, terminal requests and stty all behaved\n");
	return 0;
}
