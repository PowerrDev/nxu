/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        tools/tty/host/test_n_tty.c
 *
 * Host test of the default line discipline (kern/tty/n_tty.c), built with
 * AddressSanitizer and UndefinedBehaviorSanitizer by tools/tty/test_host.sh.
 *
 * Each case drives one discipline through its interface with a fake
 * terminal behind the hooks: an output queue whose room can be limited, a
 * record of signals and flow changes, a count of output flushes. Then a
 * randomized run feeds random bytes under random configurations with reads
 * and writes in between, checking the ring's invariants after every call.
 */

#include <kern/tty/n_tty.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE 64U
#define OUTPUT_MAX 4096U

/* ---- the fake terminal -------------------------------------------------- */

typedef struct {
	uint8_t output[OUTPUT_MAX];
	uint32_t output_length;
	uint32_t output_limit; /* room the queue reports; OUTPUT_MAX by default */
	uint32_t flushes;
	int signals[16];
	uint32_t signal_count;
	bool stopped;
	uint32_t flow_changes;
} fake_t;

static uint32_t
fake_room(void *context)
{
	fake_t *fake = context;

	return fake->output_limit - fake->output_length;
}

static void
fake_output(void *context, const uint8_t *bytes, uint32_t count)
{
	fake_t *fake = context;

	if (count > fake->output_limit - fake->output_length) {
		fprintf(stderr, "output beyond the room reported\n");
		abort();
	}

	memcpy(fake->output + fake->output_length, bytes, count);
	fake->output_length += count;
}

static void
fake_flush(void *context)
{
	fake_t *fake = context;

	fake->output_length = 0U;
	fake->flushes++;
}

static void
fake_signal(void *context, n_tty_signal_t signal)
{
	fake_t *fake = context;

	if (fake->signal_count < 16U) fake->signals[fake->signal_count++] = (int)signal;
}

static void
fake_flow(void *context, bool stopped)
{
	fake_t *fake = context;

	fake->stopped = stopped;
	fake->flow_changes++;
}

static const n_tty_hooks_t g_hooks = {
	.output_room = fake_room, .output = fake_output, .output_flush = fake_flush, .signal = fake_signal, .flow = fake_flow};

/* ---- harness ------------------------------------------------------------ */

static uint32_t g_failures;
static const char *g_case;

#define CHECK(condition)                                                                                               \
	do {                                                                                                               \
		if (!(condition)) {                                                                                            \
			fprintf(stderr, "FAIL %s (line %d): %s\n", g_case, __LINE__, #condition);                                  \
			g_failures++;                                                                                              \
		}                                                                                                              \
	} while (0)

static uint8_t g_buffer[RING_SIZE];
static uint8_t g_delimiters[RING_SIZE / 8U];
static n_tty_t g_ld;
static fake_t g_fake;

/* What Darwin's ttydefaults.h gives a new terminal, plus IUTF8. */
static struct nxu_termios
cooked(void)
{
	struct nxu_termios t;

	memset(&t, 0, sizeof(t));
	t.c_iflag = NXU_BRKINT | NXU_ICRNL | NXU_IMAXBEL | NXU_IXON | NXU_IXANY | NXU_IUTF8;
	t.c_oflag = NXU_OPOST | NXU_ONLCR;
	t.c_cflag = NXU_CREAD | NXU_CS8 | NXU_HUPCL;
	t.c_lflag = NXU_ECHO | NXU_ICANON | NXU_ISIG | NXU_IEXTEN | NXU_ECHOE | NXU_ECHOKE | NXU_ECHOCTL;
	memset(t.c_cc, NXU_POSIX_VDISABLE, sizeof(t.c_cc));
	t.c_cc[NXU_VEOF] = 0x04U;
	t.c_cc[NXU_VERASE] = 0x7FU;
	t.c_cc[NXU_VWERASE] = 0x17U;
	t.c_cc[NXU_VKILL] = 0x15U;
	t.c_cc[NXU_VREPRINT] = 0x12U;
	t.c_cc[NXU_VINTR] = 0x03U;
	t.c_cc[NXU_VQUIT] = 0x1CU;
	t.c_cc[NXU_VSUSP] = 0x1AU;
	t.c_cc[NXU_VSTART] = 0x11U;
	t.c_cc[NXU_VSTOP] = 0x13U;
	t.c_cc[NXU_VLNEXT] = 0x16U;
	t.c_cc[NXU_VDISCARD] = 0x0FU;
	t.c_cc[NXU_VMIN] = 1U;
	t.c_cc[NXU_VTIME] = 0U;
	t.c_ispeed = NXU_B9600;
	t.c_ospeed = NXU_B9600;
	return t;
}

static struct nxu_termios
raw(void)
{
	struct nxu_termios t = cooked();

	t.c_iflag = 0U;
	t.c_oflag = 0U;
	t.c_lflag = 0U;
	return t;
}

static void
start(const char *name, struct nxu_termios t)
{
	g_case = name;
	memset(&g_fake, 0, sizeof(g_fake));
	g_fake.output_limit = OUTPUT_MAX;
	n_tty_init(&g_ld, &g_hooks, &g_fake, g_buffer, g_delimiters, RING_SIZE, &t);
}

static uint32_t
feed(const char *bytes)
{
	return n_tty_receive(&g_ld, (const uint8_t *)bytes, (uint32_t)strlen(bytes));
}

/* True when the output so far is exactly `expected`; then it is cleared. */
static bool
echoed(const char *expected)
{
	size_t length = strlen(expected);
	bool match = g_fake.output_length == length && memcmp(g_fake.output, expected, length) == 0;

	if (!match) {
		fprintf(stderr, "  %s: output was \"", g_case);
		for (uint32_t index = 0U; index < g_fake.output_length; index++) {
			uint8_t c = g_fake.output[index];
			if (c >= 0x20U && c < 0x7FU)
				fputc(c, stderr);
			else
				fprintf(stderr, "\\x%02x", c);
		}
		fprintf(stderr, "\"\n");
	}

	g_fake.output_length = 0U;
	return match;
}

/* True when the next read of `capacity` returns exactly `expected` (length given, so it may hold NULs). */
static bool
reads(uint32_t capacity, const char *expected, uint32_t length)
{
	uint8_t buffer[RING_SIZE + 8U];
	uint32_t count = n_tty_read(&g_ld, buffer, capacity);

	return count == length && memcmp(buffer, expected, length) == 0;
}

#define READS(capacity, text) reads((capacity), (text), (uint32_t)(sizeof(text) - 1U))

/* ---- canonical input ---------------------------------------------------- */

static void
test_canonical(void)
{
	start("canonical line", cooked());
	CHECK(feed("abc") == 3U);
	CHECK(!n_tty_readable(&g_ld));
	CHECK(feed("\n") == 1U);
	CHECK(n_tty_readable(&g_ld));
	CHECK(echoed("abc\r\n"));
	CHECK(READS(64U, "abc\n"));
	CHECK(!n_tty_readable(&g_ld));

	start("one line per read", cooked());
	(void)feed("one\ntwo\n");
	CHECK(READS(64U, "one\n"));
	CHECK(READS(64U, "two\n"));

	start("a long line over several reads", cooked());
	(void)feed("abcde\n");
	CHECK(READS(2U, "ab"));
	CHECK(READS(2U, "cd"));
	CHECK(READS(2U, "e\n"));
	CHECK(!n_tty_readable(&g_ld));

	start("end of file", cooked());
	(void)feed("\x04");
	CHECK(n_tty_readable(&g_ld));
	CHECK(echoed(""));
	CHECK(READS(64U, ""));
	CHECK(!n_tty_readable(&g_ld));

	start("VEOF ends a line with text", cooked());
	(void)feed("ab\x04");
	CHECK(READS(64U, "ab"));
	CHECK(!n_tty_readable(&g_ld));

	start("VEOF after a line that exactly fills the read", cooked());
	(void)feed("ab\x04");
	CHECK(READS(2U, "ab"));
	CHECK(!n_tty_readable(&g_ld));

	start("VEOL ends a line", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_cc[NXU_VEOL] = ';';
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("x;y");
	CHECK(READS(64U, "x;"));
	CHECK(!n_tty_readable(&g_ld));

	start("ICRNL, IGNCR, INLCR", cooked());
	(void)feed("a\r");
	CHECK(READS(64U, "a\n"));
	{
		struct nxu_termios t = cooked();
		t.c_iflag |= NXU_IGNCR;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("b\r\n");
	CHECK(READS(64U, "b\n"));
	{
		struct nxu_termios t = raw();
		t.c_iflag = NXU_INLCR;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("\n");
	CHECK(READS(64U, "\r"));

	start("ISTRIP", raw());
	{
		struct nxu_termios t = raw();
		t.c_iflag = NXU_ISTRIP;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("\xc1");
	CHECK(READS(64U, "A"));
}

/* ---- editing ------------------------------------------------------------ */

static void
test_editing(void)
{
	start("erase", cooked());
	(void)feed("ab\x7f" "c\n");
	CHECK(echoed("ab\b \bc\r\n"));
	CHECK(READS(64U, "ac\n"));

	start("erase on an empty line does nothing", cooked());
	(void)feed("\x7f\x7f");
	CHECK(echoed(""));
	(void)feed("x\n");
	CHECK(READS(64U, "x\n"));

	start("erase never reaches into a finished line", cooked());
	(void)feed("a\n\x7f" "b\n");
	CHECK(READS(64U, "a\n"));
	CHECK(READS(64U, "b\n"));

	start("erase a tab", cooked());
	(void)feed("a\t");
	CHECK(echoed("a\t"));
	(void)feed("\x7f");
	CHECK(echoed("\b\b\b\b\b\b\b"));

	start("erase a tab after a prompt", cooked());
	(void)n_tty_write(&g_ld, (const uint8_t *)"$$$", 3U);
	(void)feed("\t\x7f");
	CHECK(echoed("$$$\t\b\b\b\b\b"));

	start("erase a control character echoed as ^X", cooked());
	(void)feed("\x01");
	CHECK(echoed("^A"));
	(void)feed("\x7f");
	CHECK(echoed("\b \b\b \b"));

	start("erase a UTF-8 sequence", cooked());
	(void)feed("a\xc3\xa9");
	CHECK(echoed("a\xc3\xa9"));
	(void)feed("\x7f\n");
	CHECK(echoed("\b \b\r\n"));
	CHECK(READS(64U, "a\n"));

	start("erase without ECHOE echoes the erase character", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag &= ~(NXU_ECHOE | NXU_ECHOCTL);
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("ab\x7f");
	CHECK(echoed("ab\x7f"));

	start("kill with ECHOKE rubs the line out", cooked());
	(void)feed("abc\x15");
	CHECK(echoed("abc\b \b\b \b\b \b"));
	(void)feed("d\n");
	CHECK(READS(64U, "d\n"));

	start("kill with ECHOK", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag = (t.c_lflag & ~NXU_ECHOKE) | NXU_ECHOK;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("abc\x15");
	CHECK(echoed("abc^U\r\n"));
	CHECK(!n_tty_readable(&g_ld));

	start("word erase", cooked());
	(void)feed("foo bar  \x17\n");
	CHECK(READS(64U, "foo \n"));

	start("word erase without IEXTEN is an ordinary byte", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag &= ~NXU_IEXTEN;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("a\x17\n");
	CHECK(READS(64U, "a\x17\n"));

	start("reprint", cooked());
	(void)feed("ab");
	g_fake.output_length = 0U;
	(void)feed("\x12");
	CHECK(echoed("^R\r\nab"));
	(void)feed("\x7f");
	CHECK(echoed("\b \b"));

	start("literal next", cooked());
	(void)feed("\x16\x03\x16\x7f\n");
	CHECK(g_fake.signal_count == 0U);
	CHECK(echoed("^\b^C^\b^?\r\n"));
	CHECK(READS(64U, "\x03\x7f\n"));
}

/* ---- echo --------------------------------------------------------------- */

static void
test_echo(void)
{
	start("no echo", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag &= ~NXU_ECHO;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("secret\n");
	CHECK(echoed(""));
	CHECK(READS(64U, "secret\n"));

	start("ECHONL echoes only the newline", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag = (t.c_lflag & ~NXU_ECHO) | NXU_ECHONL;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("pw\n");
	CHECK(echoed("\r\n"));

	start("echo that does not fit is dropped", cooked());
	g_fake.output_limit = 2U;
	(void)feed("abcd\n");
	CHECK(echoed("ab"));
	CHECK(READS(64U, "abcd\n"));
}

/* ---- special characters ------------------------------------------------- */

static void
test_signals(void)
{
	start("interrupt", cooked());
	(void)feed("typed");
	(void)n_tty_write(&g_ld, (const uint8_t *)"pending", 7U);
	(void)feed("\x03");
	CHECK(g_fake.signal_count == 1U && g_fake.signals[0] == N_TTY_SIGNAL_INTERRUPT);
	CHECK(g_fake.flushes == 1U);
	CHECK(echoed("^C"));
	(void)feed("x\n");
	CHECK(READS(64U, "x\n"));

	start("quit and suspend", cooked());
	(void)feed("\x1c\x1a");
	CHECK(g_fake.signal_count == 2U && g_fake.signals[0] == N_TTY_SIGNAL_QUIT &&
		  g_fake.signals[1] == N_TTY_SIGNAL_SUSPEND);

	start("NOFLSH keeps input", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag |= NXU_NOFLSH;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("keep\x03\n");
	CHECK(g_fake.flushes == 0U);
	CHECK(READS(64U, "keep\n"));

	start("no ISIG", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag &= ~NXU_ISIG;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("\x03\n");
	CHECK(g_fake.signal_count == 0U);
	CHECK(READS(64U, "\x03\n"));

	start("stop and start", cooked());
	(void)feed("\x13");
	CHECK(g_fake.stopped && g_ld.stopped);
	(void)feed("\x11");
	CHECK(!g_fake.stopped && g_fake.flow_changes == 2U);
	CHECK(!n_tty_readable(&g_ld) && g_ld.head == g_ld.read_tail);

	start("IXANY restarts on any byte", cooked());
	(void)feed("\x13" "a");
	CHECK(!g_fake.stopped);

	start("without IXANY only VSTART restarts", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_iflag &= ~NXU_IXANY;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("\x13" "a");
	CHECK(g_fake.stopped);

	start("turning IXON off restarts output", cooked());
	(void)feed("\x13");
	{
		struct nxu_termios t = cooked();
		t.c_iflag &= ~NXU_IXON;
		n_tty_set_termios(&g_ld, &t);
	}
	CHECK(!g_fake.stopped);

	start("an interrupt restarts stopped output", cooked());
	(void)feed("\x13\x03");
	CHECK(!g_fake.stopped);

	start("discard", cooked());
	(void)feed("\x0f");
	CHECK(echoed("^O"));
	CHECK((g_ld.termios.c_lflag & NXU_FLUSHO) != 0U);
	CHECK(n_tty_write(&g_ld, (const uint8_t *)"lost", 4U) == 4U);
	CHECK(echoed(""));
	(void)feed("\x0f");
	CHECK((g_ld.termios.c_lflag & NXU_FLUSHO) == 0U);
	CHECK(n_tty_write(&g_ld, (const uint8_t *)"kept", 4U) == 4U);
	CHECK(echoed("kept"));
}

/* ---- raw input, room, modes --------------------------------------------- */

static void
test_raw_and_room(void)
{
	start("raw bytes are readable at once", raw());
	(void)feed("ab");
	CHECK(n_tty_readable(&g_ld) && n_tty_available(&g_ld) == 2U);
	CHECK(READS(1U, "a"));
	CHECK(READS(64U, "b"));
	CHECK(echoed(""));

	start("raw input refuses what does not fit", raw());
	{
		char many[RING_SIZE + 11U];
		memset(many, 'r', sizeof(many) - 1U);
		many[sizeof(many) - 1U] = '\0';
		CHECK(feed(many) == RING_SIZE);
		CHECK(n_tty_receive_room(&g_ld) == 0U);
	}
	CHECK(n_tty_read(&g_ld, (uint8_t[8]){0}, 8U) == 8U);
	CHECK(n_tty_receive_room(&g_ld) == 8U);

	start("a full canonical line drops, rings and still ends", cooked());
	{
		char many[RING_SIZE + 6U];
		memset(many, 'c', sizeof(many) - 1U);
		many[sizeof(many) - 1U] = '\0';
		CHECK(feed(many) == RING_SIZE + 5U);
	}
	CHECK(g_fake.output_length > RING_SIZE - 1U && g_fake.output[g_fake.output_length - 1U] == 0x07U);
	CHECK(feed("\n") == 1U);
	CHECK(n_tty_readable(&g_ld));
	CHECK(n_tty_available(&g_ld) == RING_SIZE);

	start("leaving canonical mode makes the line readable", cooked());
	(void)feed("part");
	n_tty_set_termios(&g_ld, &(struct nxu_termios){0});
	{
		struct nxu_termios t = raw();
		n_tty_set_termios(&g_ld, &t);
	}
	CHECK(READS(64U, "part"));

	start("entering canonical mode makes unread input editable", raw());
	(void)feed("xy");
	{
		struct nxu_termios t = cooked();
		n_tty_set_termios(&g_ld, &t);
	}
	CHECK(!n_tty_readable(&g_ld));
	(void)feed("\x7f\n");
	CHECK(READS(64U, "x\n"));

	start("flush input", cooked());
	(void)feed("done\nhalf");
	n_tty_flush_input(&g_ld);
	CHECK(!n_tty_readable(&g_ld) && n_tty_available(&g_ld) == 0U);
	(void)feed("\x7f" "z\n");
	CHECK(READS(64U, "z\n"));
}

/* ---- output processing -------------------------------------------------- */

static uint32_t
write_text(const char *text)
{
	return n_tty_write(&g_ld, (const uint8_t *)text, (uint32_t)strlen(text));
}

static void
set_oflag(nxu_tcflag_t oflag)
{
	struct nxu_termios t = cooked();

	t.c_oflag = oflag;
	n_tty_set_termios(&g_ld, &t);
}

static void
test_output(void)
{
	start("ONLCR", cooked());
	CHECK(write_text("a\nb") == 3U);
	CHECK(echoed("a\r\nb"));

	start("no OPOST passes bytes through", cooked());
	set_oflag(NXU_ONLCR);
	(void)write_text("a\n\t");
	CHECK(echoed("a\n\t"));

	start("OCRNL and ONLRET", cooked());
	set_oflag(NXU_OPOST | NXU_OCRNL);
	(void)write_text("x\r");
	CHECK(echoed("x\n"));

	start("ONOCR drops a carriage return at column 0", cooked());
	set_oflag(NXU_OPOST | NXU_ONOCR);
	CHECK(write_text("\rab\r") == 4U);
	CHECK(echoed("ab\r"));

	start("OXTABS", cooked());
	set_oflag(NXU_OPOST | NXU_OXTABS);
	(void)write_text("ab\tc");
	CHECK(echoed("ab      c"));

	start("ONOEOT", cooked());
	set_oflag(NXU_OPOST | NXU_ONOEOT);
	CHECK(write_text("a\x04" "b") == 3U);
	CHECK(echoed("ab"));

	start("a write stops before what does not fit", cooked());
	g_fake.output_limit = 4U;
	CHECK(write_text("abc\n") == 3U);
	CHECK(echoed("abc"));
	g_fake.output_limit = 4U;
	CHECK(write_text("\n") == 1U);
	CHECK(echoed("\r\n"));
}

/* ---- line errors -------------------------------------------------------- */

static struct nxu_termios
with_iflag(struct nxu_termios t, nxu_tcflag_t iflag)
{
	t.c_iflag = iflag;
	return t;
}

static bool
error_in(uint8_t c, n_tty_error_t error)
{
	return n_tty_receive_error(&g_ld, c, error);
}

static void
test_line_errors(void)
{
	start("IGNBRK drops a break", with_iflag(raw(), NXU_IGNBRK | NXU_BRKINT));
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(n_tty_available(&g_ld) == 0U && g_fake.signal_count == 0U);

	start("BRKINT flushes both ways and interrupts", cooked());
	(void)feed("half");
	(void)echoed("half");
	(void)write_text("out");
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(g_fake.signal_count == 1U && g_fake.signals[0] == (int)N_TTY_SIGNAL_INTERRUPT);
	CHECK(g_fake.flushes == 1U && echoed(""));
	(void)feed("\n");
	CHECK(READS(64U, "\n"));

	start("BRKINT under NOFLSH keeps the queues", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_lflag |= NXU_NOFLSH;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("keep");
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(g_fake.signal_count == 1U && g_fake.flushes == 0U);
	(void)feed("\n");
	CHECK(READS(64U, "keep\n"));

	start("BRKINT restarts stopped output", cooked());
	(void)feed("\x13");
	CHECK(g_fake.stopped);
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(!g_fake.stopped);

	start("a break reads as NUL", raw());
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(reads(64U, "\0", 1U));

	start("a break under PARMRK reads as 377 0 0, even with IGNPAR", with_iflag(raw(), NXU_PARMRK | NXU_IGNPAR));
	CHECK(error_in(0U, N_TTY_ERROR_BREAK));
	CHECK(reads(64U, "\377\0\0", 3U));

	start("parity is not checked without INPCK", raw());
	CHECK(error_in('p', N_TTY_ERROR_PARITY));
	CHECK(error_in('f', N_TTY_ERROR_FRAMING));
	CHECK(READS(64U, "pf"));

	start("unchecked bytes are still special", cooked());
	CHECK(error_in(0x03U, N_TTY_ERROR_PARITY));
	CHECK(g_fake.signal_count == 1U);

	start("INPCK and IGNPAR drop bad bytes", with_iflag(raw(), NXU_INPCK | NXU_IGNPAR | NXU_PARMRK));
	CHECK(error_in('x', N_TTY_ERROR_PARITY));
	CHECK(error_in('y', N_TTY_ERROR_FRAMING));
	CHECK(n_tty_available(&g_ld) == 0U);

	start("INPCK and PARMRK mark bad bytes", with_iflag(raw(), NXU_INPCK | NXU_PARMRK));
	CHECK(error_in('x', N_TTY_ERROR_PARITY));
	CHECK(error_in(0x03U, N_TTY_ERROR_FRAMING));
	CHECK(reads(64U, "\377\0x\377\0\x03", 6U));

	start("INPCK alone reads bad bytes as NUL", with_iflag(raw(), NXU_INPCK));
	CHECK(error_in('x', N_TTY_ERROR_PARITY));
	CHECK(reads(64U, "\0", 1U));

	start("PARMRK doubles a real 377", with_iflag(raw(), NXU_PARMRK));
	(void)n_tty_receive(&g_ld, (const uint8_t[]){'a', 0xFFU, 'b'}, 3U);
	CHECK(reads(64U, "a\377\377b", 4U));

	start("no doubling under IGNPAR or ISTRIP", with_iflag(raw(), NXU_PARMRK | NXU_IGNPAR));
	(void)n_tty_receive(&g_ld, (const uint8_t[]){0xFFU}, 1U);
	CHECK(reads(64U, "\377", 1U));
	n_tty_set_termios(&g_ld, &(struct nxu_termios){.c_iflag = NXU_PARMRK | NXU_ISTRIP});
	(void)n_tty_receive(&g_ld, (const uint8_t[]){0xFFU}, 1U);
	CHECK(reads(64U, "\x7f", 1U));

	start("a mark is refused whole when it does not fit", with_iflag(raw(), NXU_INPCK | NXU_PARMRK));
	{
		char many[RING_SIZE - 1U];
		memset(many, 'r', sizeof(many) - 1U);
		many[sizeof(many) - 1U] = '\0';
		CHECK(feed(many) == RING_SIZE - 2U);
	}
	CHECK(!error_in('x', N_TTY_ERROR_PARITY));
	CHECK(n_tty_available(&g_ld) == RING_SIZE - 2U);
	CHECK(n_tty_read(&g_ld, (uint8_t[8]){0}, 1U) == 1U);
	CHECK(error_in('x', N_TTY_ERROR_PARITY));
	CHECK(n_tty_receive_room(&g_ld) == 0U);

	start("a doubled 377 is refused whole too", with_iflag(raw(), NXU_PARMRK));
	{
		char many[RING_SIZE];
		memset(many, 'r', sizeof(many) - 1U);
		many[sizeof(many) - 1U] = '\0';
		CHECK(feed(many) == RING_SIZE - 1U);
	}
	CHECK(n_tty_receive(&g_ld, (const uint8_t[]){0xFFU}, 1U) == 0U);
	CHECK(n_tty_available(&g_ld) == RING_SIZE - 1U);

	start("a mark in a canonical line is erased byte by byte", cooked());
	{
		struct nxu_termios t = cooked();
		t.c_iflag |= NXU_INPCK | NXU_PARMRK;
		n_tty_set_termios(&g_ld, &t);
	}
	(void)feed("a");
	CHECK(error_in('x', N_TTY_ERROR_PARITY));
	(void)feed("\x7f\n");
	CHECK(reads(64U, "a\377\0\n", 4U));
}

/* ---- randomized --------------------------------------------------------- */

static uint32_t g_seed = 0x12345678U;

static uint32_t
random_next(void)
{
	g_seed ^= g_seed << 13U;
	g_seed ^= g_seed >> 17U;
	g_seed ^= g_seed << 5U;
	return g_seed;
}

static void
check_invariants(void)
{
	uint32_t used = g_ld.head - g_ld.read_tail;
	uint32_t ready = g_ld.canon_head - g_ld.read_tail;

	CHECK(used <= RING_SIZE);
	CHECK(ready <= used);
	CHECK((g_ld.termios.c_lflag & NXU_ICANON) != 0U || g_ld.canon_head == g_ld.head);
}

static void
test_random(void)
{
	start("randomized", cooked());

	/* Bytes biased toward the ones that mean something. */
	static const uint8_t special[] = {'\n', '\r', '\t', 0x03, 0x04, 0x0f, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17,
									  0x1a, 0x1c, 0x7f, 0xc3, 0xa9, 0x80, 0xff, ' ', 'a', 'b'};

	for (uint32_t round = 0U; round < 300000U && g_failures == 0U; round++) {
		uint32_t action = random_next() % 16U;

		if (action < 9U) {
			uint8_t bytes[24];
			uint32_t count = random_next() % sizeof(bytes);

			for (uint32_t index = 0U; index < count; index++) {
				bytes[index] = random_next() % 3U == 0U ? (uint8_t)random_next()
														 : special[random_next() % sizeof(special)];
			}

			CHECK(n_tty_receive(&g_ld, bytes, count) <= count);
		} else if (action < 12U) {
			uint8_t buffer[RING_SIZE + 8U];
			uint32_t capacity = random_next() % sizeof(buffer);

			CHECK(n_tty_read(&g_ld, buffer, capacity) <= capacity);
		} else if (action < 13U && random_next() % 2U == 0U) {
			(void)n_tty_receive_error(&g_ld, special[random_next() % sizeof(special)],
									  (n_tty_error_t)(random_next() % 3U));
		} else if (action < 13U) {
			uint8_t bytes[16];
			uint32_t count = random_next() % sizeof(bytes);

			for (uint32_t index = 0U; index < count; index++)
				bytes[index] = special[random_next() % sizeof(special)];

			CHECK(n_tty_write(&g_ld, bytes, count) <= count);
		} else if (action < 14U) {
			struct nxu_termios t = cooked();

			t.c_iflag = random_next();
			t.c_oflag = random_next();
			t.c_lflag = random_next();
			if (random_next() % 4U == 0U) t.c_cc[random_next() % NXU_NCCS] = (uint8_t)random_next();

			n_tty_set_termios(&g_ld, &t);
		} else if (action < 15U) {
			g_fake.output_limit = g_fake.output_length + random_next() % 12U;
		} else {
			g_fake.output_length = 0U;
			g_fake.output_limit = OUTPUT_MAX;
		}

		if (g_fake.output_length > OUTPUT_MAX / 2U) {
			g_fake.output_length = 0U;
			g_fake.output_limit = OUTPUT_MAX;
		}

		check_invariants();
	}
}

int
main(void)
{
	test_canonical();
	test_editing();
	test_echo();
	test_signals();
	test_raw_and_room();
	test_output();
	test_line_errors();
	test_random();

	if (g_failures != 0U) {
		fprintf(stderr, "n_tty: %u failure(s)\n", g_failures);
		return 1;
	}

	printf("n_tty: all cases passed\n");
	return 0;
}
