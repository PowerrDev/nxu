/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/BootDaemons.framework/stty.c
 *
 * stty: show and change a terminal's settings.
 *
 *     stty [-a | -g] [-f file] [operand ...]
 *
 * The BSD stty, as macOS ships it, in what it prints and what it accepts.
 *
 * With no operands it prints the settings: the speed, then every mode and
 * control character that differs from a new terminal's (-a: all of them,
 * with the window size). -g prints them as one line that stty accepts back
 * as an operand, to save and restore a terminal's state:
 *
 *     saved=$(stty -g); stty raw -echo; ...; stty "$saved"
 *
 * Operands, applied left to right and then set in one step:
 *
 *     [-]mode               a mode by name (echo, icanon, ixon, opost, ...)
 *     cs5 cs6 cs7 cs8       character size
 *     <cchar> <value>       intr ^C, erase ^?, eof undef (or ^-), kill x
 *     min <n>, time <n>     VMIN and VTIME
 *     <number>, speed <n>   both speeds; ispeed <n>, ospeed <n> one of them
 *     rows <n>, columns <n> (cols) the window size
 *     size, speed           print the window size, the speed
 *     raw, -raw, cooked, cbreak, -cbreak, sane, nl, -nl, ek, crt, newcrt,
 *     dec, evenp, oddp, parity, -evenp, -oddp, -parity, pass8, -pass8,
 *     litout, -litout       the usual combinations
 *     gfmt1:...             a line printed by -g
 *
 * The terminal is standard input, or `file` (opened without waiting, so a
 * terminal with no carrier can still be configured).
 */
#include <termios.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/* ---- output ------------------------------------------------------------- */

static char g_output[4096];
static uint32_t g_output_length;

static void
out_flush(void)
{
	if (g_output_length != 0U) (void)nxu_write(1U, g_output, g_output_length);

	g_output_length = 0U;
}

/* Append; returns the number of bytes, as printf would, for column counting. */
static uint32_t
out(const char *text)
{
	uint32_t length = (uint32_t)nxu_strlen(text);

	for (uint32_t index = 0U; index < length; index++) {
		if (g_output_length == sizeof(g_output)) out_flush();

		g_output[g_output_length++] = text[index];
	}

	return length;
}

static void
decimal(uint64_t value, char text[24])
{
	char reversed[24];
	uint32_t length = 0U;

	do {
		reversed[length++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);

	for (uint32_t index = 0U; index < length; index++)
		text[index] = reversed[length - 1U - index];

	text[length] = '\0';
}

static void
hex(uint64_t value, char text[24])
{
	static const char digits[] = "0123456789abcdef";
	char reversed[24];
	uint32_t length = 0U;

	do {
		reversed[length++] = digits[value & 0xFU];
		value >>= 4U;
	} while (value != 0U);

	for (uint32_t index = 0U; index < length; index++)
		text[index] = reversed[length - 1U - index];

	text[length] = '\0';
}

static uint32_t
out_decimal(uint64_t value)
{
	char text[24];

	decimal(value, text);
	return out(text);
}

static void
warn(const char *first, const char *second)
{
	out_flush();
	(void)nxu_write(2U, "stty: ", 6U);
	(void)nxu_write(2U, first, nxu_strlen(first));
	if (second != 0) (void)nxu_write(2U, second, nxu_strlen(second));
	(void)nxu_write(2U, "\n", 1U);
}

/*
 * Labelled, wrapped lists, as BSD's print.c lays them out: "label: a b c",
 * continued on lines that start with a tab once 72 columns would be passed.
 */
#define STTY_WIDTH 72U

static const char *g_label;
static uint32_t g_column;

static void
list_begin(const char *label)
{
	if (g_column != 0U) out("\n");

	g_column = 0U;
	g_label = label;
}

static void
list_put(const char *item)
{
	if (g_column == 0U) {
		g_column = out(g_label) + out(": ") + out(item);
		return;
	}

	if (g_column + nxu_strlen(item) > STTY_WIDTH) {
		out("\n\t");
		g_column = out(item) + 8U;
		return;
	}

	g_column += out(" ") + out(item);
}

/* ---- the tables --------------------------------------------------------- */

typedef enum { FIELD_IFLAG, FIELD_OFLAG, FIELD_CFLAG, FIELD_LFLAG } field_t;

static tcflag_t *
field_of(struct termios *termios, field_t field)
{
	switch (field) {
	case FIELD_IFLAG:
		return &termios->c_iflag;
	case FIELD_OFLAG:
		return &termios->c_oflag;
	case FIELD_CFLAG:
		return &termios->c_cflag;
	default:
		return &termios->c_lflag;
	}
}

/* The modes stty prints, by group, in BSD's order; each is one bit. */
typedef struct {
	const char *name;
	field_t field;
	tcflag_t bit;
} stty_mode_t;

static const stty_mode_t g_modes[] = {
	{"icanon", FIELD_LFLAG, ICANON},	   {"isig", FIELD_LFLAG, ISIG},
	{"iexten", FIELD_LFLAG, IEXTEN},	   {"echo", FIELD_LFLAG, ECHO},
	{"echoe", FIELD_LFLAG, ECHOE},		   {"echok", FIELD_LFLAG, ECHOK},
	{"echoke", FIELD_LFLAG, ECHOKE},	   {"echonl", FIELD_LFLAG, ECHONL},
	{"echoctl", FIELD_LFLAG, ECHOCTL},	   {"echoprt", FIELD_LFLAG, ECHOPRT},
	{"altwerase", FIELD_LFLAG, ALTWERASE}, {"noflsh", FIELD_LFLAG, NOFLSH},
	{"tostop", FIELD_LFLAG, TOSTOP},	   {"flusho", FIELD_LFLAG, FLUSHO},
	{"pendin", FIELD_LFLAG, PENDIN},	   {"nokerninfo", FIELD_LFLAG, NOKERNINFO},
	{"extproc", FIELD_LFLAG, EXTPROC},

	{"istrip", FIELD_IFLAG, ISTRIP},	   {"icrnl", FIELD_IFLAG, ICRNL},
	{"inlcr", FIELD_IFLAG, INLCR},		   {"igncr", FIELD_IFLAG, IGNCR},
	{"ixon", FIELD_IFLAG, IXON},		   {"ixoff", FIELD_IFLAG, IXOFF},
	{"ixany", FIELD_IFLAG, IXANY},		   {"imaxbel", FIELD_IFLAG, IMAXBEL},
	{"iutf8", FIELD_IFLAG, IUTF8},		   {"ignbrk", FIELD_IFLAG, IGNBRK},
	{"brkint", FIELD_IFLAG, BRKINT},	   {"inpck", FIELD_IFLAG, INPCK},
	{"ignpar", FIELD_IFLAG, IGNPAR},	   {"parmrk", FIELD_IFLAG, PARMRK},

	{"opost", FIELD_OFLAG, OPOST},		   {"onlcr", FIELD_OFLAG, ONLCR},
	{"ocrnl", FIELD_OFLAG, OCRNL},		   {"oxtabs", FIELD_OFLAG, OXTABS},
	{"onocr", FIELD_OFLAG, ONOCR},		   {"onlret", FIELD_OFLAG, ONLRET},
	{"onoeot", FIELD_OFLAG, ONOEOT},

	{"cread", FIELD_CFLAG, CREAD},		   {"parenb", FIELD_CFLAG, PARENB},
	{"parodd", FIELD_CFLAG, PARODD},	   {"hupcl", FIELD_CFLAG, HUPCL},
	{"clocal", FIELD_CFLAG, CLOCAL},	   {"cstopb", FIELD_CFLAG, CSTOPB},
};

#define MODE_COUNT (sizeof(g_modes) / sizeof(g_modes[0]))

static const char *const g_field_labels[] = {"iflags", "oflags", "cflags", "lflags"};

/* The control characters, alphabetically, as BSD prints and -g writes them. */
typedef struct {
	const char *name;
	uint32_t slot;
	cc_t initial;
} stty_cchar_t;

static const stty_cchar_t g_cchars[] = {
	{"discard", VDISCARD, NXU_CDISCARD}, {"dsusp", VDSUSP, NXU_CDSUSP},		{"eof", VEOF, NXU_CEOF},
	{"eol", VEOL, NXU_CEOL},			 {"eol2", VEOL2, NXU_CEOL2},		{"erase", VERASE, NXU_CERASE},
	{"intr", VINTR, NXU_CINTR},			 {"kill", VKILL, NXU_CKILL},		{"lnext", VLNEXT, NXU_CLNEXT},
	{"min", VMIN, NXU_CMIN},			 {"quit", VQUIT, NXU_CQUIT},		{"reprint", VREPRINT, NXU_CREPRINT},
	{"start", VSTART, NXU_CSTART},		 {"status", VSTATUS, NXU_CSTATUS},	{"stop", VSTOP, NXU_CSTOP},
	{"susp", VSUSP, NXU_CSUSP},			 {"time", VTIME, NXU_CTIME},		{"werase", VWERASE, NXU_CWERASE},
};

#define CCHAR_COUNT (sizeof(g_cchars) / sizeof(g_cchars[0]))

static tcflag_t
initial_field(field_t field)
{
	switch (field) {
	case FIELD_IFLAG:
		return NXU_TTYDEF_IFLAG;
	case FIELD_OFLAG:
		return NXU_TTYDEF_OFLAG;
	case FIELD_CFLAG:
		return NXU_TTYDEF_CFLAG;
	default:
		return NXU_TTYDEF_LFLAG;
	}
}

/* ---- printing ----------------------------------------------------------- */

/* "^C", "^?", "M-^C", "<undef>", or the character; min and time as numbers. */
static void
cchar_text(const stty_cchar_t *cchar, cc_t value, char text[16])
{
	uint32_t length = 0U;

	if (cchar->slot == VMIN || cchar->slot == VTIME) {
		decimal(value, text);
		return;
	}

	if (value == _POSIX_VDISABLE) {
		memcpy(text, "<undef>", 8U);
		return;
	}

	if ((value & 0x80U) != 0U) {
		text[length++] = 'M';
		text[length++] = '-';
		value &= 0x7FU;
	}

	if (value == 0x7FU) {
		text[length++] = '^';
		text[length++] = '?';
	} else if (value < 0x20U) {
		text[length++] = '^';
		text[length++] = (char)(value + '@');
	} else {
		text[length++] = (char)value;
	}

	text[length] = '\0';
}

static void
print_settings(struct termios *termios, const struct winsize *size, bool all)
{
	if (termios->c_ispeed != termios->c_ospeed) {
		out("ispeed ");
		out_decimal(termios->c_ispeed);
		out(" baud; ospeed ");
		out_decimal(termios->c_ospeed);
		out(" baud;");
	} else {
		out("speed ");
		out_decimal(termios->c_ospeed);
		out(" baud;");
	}

	if (all) {
		out(" ");
		out_decimal(size->ws_row);
		out(" rows; ");
		out_decimal(size->ws_col);
		out(" columns;");
	}

	out("\n");

	static const field_t order[] = {FIELD_LFLAG, FIELD_IFLAG, FIELD_OFLAG, FIELD_CFLAG};

	for (uint32_t group = 0U; group < 4U; group++) {
		field_t field = order[group];
		tcflag_t value = *field_of(termios, field);
		tcflag_t initial = initial_field(field);

		list_begin(g_field_labels[field]);

		/* The character size reads as one mode, first among the cflags. */
		if (field == FIELD_CFLAG && (all || (value & CSIZE) != (initial & CSIZE))) {
			static const char *const sizes[] = {"cs5", "cs6", "cs7", "cs8"};

			list_put(sizes[(value & CSIZE) >> 8U]);
		}

		for (uint32_t index = 0U; index < MODE_COUNT; index++) {
			const stty_mode_t *mode = &g_modes[index];

			if (mode->field != field) continue;

			bool on = (value & mode->bit) != 0U;
			bool initially = (initial & mode->bit) != 0U;

			if (!all && on == initially) continue;

			char item[24] = "-";

			memcpy(item + 1, mode->name, nxu_strlen(mode->name) + 1U);
			list_put(on ? item + 1 : item);
		}
	}

	list_begin("cchars");

	for (uint32_t index = 0U; index < CCHAR_COUNT; index++) {
		const stty_cchar_t *cchar = &g_cchars[index];
		cc_t value = termios->c_cc[cchar->slot];

		if (!all && value == cchar->initial) continue;

		char item[40];
		char text[16];
		uint32_t length = (uint32_t)nxu_strlen(cchar->name);

		cchar_text(cchar, value, text);
		memcpy(item, cchar->name, length);
		memcpy(item + length, " = ", 3U);
		length += 3U;
		memcpy(item + length, text, nxu_strlen(text));
		length += (uint32_t)nxu_strlen(text);
		item[length++] = ';';
		item[length] = '\0';
		list_put(item);
	}

	list_begin(0);
}

/* gfmt1:cflag=4b00:iflag=6b02:...:ispeed=9600:ospeed=9600 */
static void
print_saved(const struct termios *termios)
{
	char text[24];
	static const char *const names[] = {"cflag", "iflag", "lflag", "oflag"};
	tcflag_t values[] = {termios->c_cflag, termios->c_iflag, termios->c_lflag, termios->c_oflag};

	out("gfmt1");

	for (uint32_t index = 0U; index < 4U; index++) {
		out(":");
		out(names[index]);
		out("=");
		hex(values[index], text);
		out(text);
	}

	for (uint32_t index = 0U; index < CCHAR_COUNT; index++) {
		out(":");
		out(g_cchars[index].name);
		out("=");
		hex(termios->c_cc[g_cchars[index].slot], text);
		out(text);
	}

	out(":ispeed=");
	out_decimal(termios->c_ispeed);
	out(":ospeed=");
	out_decimal(termios->c_ospeed);
	out("\n");
}

/* ---- parsing ------------------------------------------------------------ */

static bool
parse_number(const char *text, uint32_t base, uint64_t limit, uint64_t *result)
{
	uint64_t value = 0U;

	if (*text == '\0') return false;

	for (; *text != '\0'; text++) {
		uint32_t digit;

		if (*text >= '0' && *text <= '9')
			digit = (uint32_t)(*text - '0');
		else if (base == 16U && *text >= 'a' && *text <= 'f')
			digit = (uint32_t)(*text - 'a' + 10);
		else if (base == 16U && *text >= 'A' && *text <= 'F')
			digit = (uint32_t)(*text - 'A' + 10);
		else
			return false;

		if (digit >= base) return false;

		value = value * base + digit;
		if (value > limit) return false;
	}

	*result = value;
	return true;
}

/* "^C", "^?", "^-", "undef", or one character. */
static bool
parse_cchar(const char *text, cc_t *value)
{
	if (nxu_streq(text, "undef") || nxu_streq(text, "^-")) {
		*value = _POSIX_VDISABLE;
		return true;
	}

	if (text[0] == '^' && text[1] != '\0' && text[2] == '\0') {
		*value = text[1] == '?' ? 0x7FU : (cc_t)(text[1] & 0x1FU);
		return true;
	}

	if (text[0] != '\0' && text[1] == '\0') {
		*value = (cc_t)text[0];
		return true;
	}

	return false;
}

/* A line printed by -g. Every field must be there and valid; the terminal is changed only if all are. */
static bool
parse_saved(const char *text, struct termios *termios)
{
	struct termios parsed = *termios;
	uint32_t seen = 0U;

	if (!(text[0] == 'g' && text[1] == 'f' && text[2] == 'm' && text[3] == 't' && text[4] == '1' && text[5] == ':'))
		return false;

	text += 6;

	while (*text != '\0') {
		char key[16];
		char value[24];
		uint32_t length = 0U;

		while (*text != '=' && *text != '\0' && length < sizeof(key) - 1U)
			key[length++] = *text++;
		key[length] = '\0';
		if (*text++ != '=') return false;

		length = 0U;
		while (*text != ':' && *text != '\0' && length < sizeof(value) - 1U)
			value[length++] = *text++;
		value[length] = '\0';
		if (*text == ':') text++;

		uint64_t number;
		bool speed = nxu_streq(key, "ispeed") || nxu_streq(key, "ospeed");

		if (!parse_number(value, speed ? 10U : 16U, UINT32_MAX, &number)) return false;

		if (nxu_streq(key, "cflag"))
			parsed.c_cflag = (tcflag_t)number;
		else if (nxu_streq(key, "iflag"))
			parsed.c_iflag = (tcflag_t)number;
		else if (nxu_streq(key, "lflag"))
			parsed.c_lflag = (tcflag_t)number;
		else if (nxu_streq(key, "oflag"))
			parsed.c_oflag = (tcflag_t)number;
		else if (nxu_streq(key, "ispeed"))
			parsed.c_ispeed = (speed_t)number;
		else if (nxu_streq(key, "ospeed"))
			parsed.c_ospeed = (speed_t)number;
		else {
			uint32_t index = 0U;

			while (index < CCHAR_COUNT && !nxu_streq(key, g_cchars[index].name))
				index++;

			if (index == CCHAR_COUNT || number > 0xFFU) return false;

			parsed.c_cc[g_cchars[index].slot] = (cc_t)number;
		}

		seen++;
	}

	if (seen != 6U + CCHAR_COUNT) return false;

	*termios = parsed;
	return true;
}

/* The combinations, after BSD's key.c. True when `word` was one. */
static bool
apply_combination(const char *word, struct termios *t)
{
	bool off = word[0] == '-';
	const char *name = off ? word + 1 : word;

	if (nxu_streq(word, "raw")) {
		cfmakeraw(t);
		return true;
	}

	if (nxu_streq(word, "-raw") || nxu_streq(word, "cooked")) {
		t->c_iflag |= BRKINT | ICRNL | IMAXBEL | IXON;
		t->c_oflag |= OPOST;
		t->c_lflag |= ISIG | ICANON | IEXTEN;
		return true;
	}

	if (nxu_streq(word, "sane") || nxu_streq(word, "-cbreak")) {
		t->c_cflag = NXU_TTYDEF_CFLAG | (t->c_cflag & CLOCAL);
		t->c_iflag = NXU_TTYDEF_IFLAG;
		t->c_oflag = NXU_TTYDEF_OFLAG;
		t->c_lflag = NXU_TTYDEF_LFLAG;
		return true;
	}

	if (nxu_streq(word, "cbreak")) {
		t->c_iflag |= BRKINT | IXON | IMAXBEL;
		t->c_oflag |= OPOST;
		t->c_lflag |= ISIG | IEXTEN;
		t->c_lflag &= ~ICANON;
		return true;
	}

	if (nxu_streq(name, "nl")) {
		if (off) {
			t->c_iflag |= ICRNL;
			t->c_oflag |= ONLCR;
		} else {
			t->c_iflag &= ~ICRNL;
			t->c_oflag &= ~ONLCR;
		}

		return true;
	}

	if (nxu_streq(word, "ek")) {
		t->c_cc[VERASE] = NXU_CERASE;
		t->c_cc[VKILL] = NXU_CKILL;
		return true;
	}

	if (nxu_streq(word, "crt") || nxu_streq(word, "newcrt")) {
		t->c_lflag |= ECHOE | ECHOKE | ECHOCTL;
		return true;
	}

	if (nxu_streq(word, "dec")) {
		t->c_lflag |= ECHOE | ECHOKE | ECHOCTL;
		t->c_iflag &= ~IXANY;
		t->c_cc[VERASE] = 0x7FU;
		t->c_cc[VKILL] = NXU_CTRL('u');
		t->c_cc[VINTR] = NXU_CTRL('c');
		return true;
	}

	if (nxu_streq(name, "evenp") || nxu_streq(name, "parity") || nxu_streq(name, "oddp")) {
		t->c_cflag &= ~(CSIZE | PARENB | PARODD);

		if (off) {
			t->c_cflag |= CS8;
		} else {
			t->c_cflag |= PARENB | CS7;
			if (nxu_streq(name, "oddp")) t->c_cflag |= PARODD;
		}

		return true;
	}

	if (nxu_streq(name, "pass8")) {
		t->c_cflag &= ~(CSIZE | PARENB);

		if (off) {
			t->c_cflag |= PARENB | CS7;
			t->c_iflag |= ISTRIP;
		} else {
			t->c_cflag |= CS8;
			t->c_iflag &= ~ISTRIP;
		}

		return true;
	}

	if (nxu_streq(name, "litout")) {
		t->c_cflag &= ~(CSIZE | PARENB);

		if (off) {
			t->c_cflag |= PARENB | CS7;
			t->c_oflag |= OPOST;
		} else {
			t->c_cflag |= CS8;
			t->c_oflag &= ~OPOST;
		}

		return true;
	}

	static const char *const sizes[] = {"cs5", "cs6", "cs7", "cs8"};

	for (uint32_t index = 0U; index < 4U; index++) {
		if (nxu_streq(word, sizes[index])) {
			t->c_cflag = (t->c_cflag & ~CSIZE) | ((tcflag_t)index << 8U);
			return true;
		}
	}

	return false;
}

/* [-]mode. */
static bool
apply_mode(const char *word, struct termios *t)
{
	bool off = word[0] == '-';
	const char *name = off ? word + 1 : word;

	for (uint32_t index = 0U; index < MODE_COUNT; index++) {
		const stty_mode_t *mode = &g_modes[index];

		if (!nxu_streq(name, mode->name)) continue;

		tcflag_t *field = field_of(t, mode->field);

		*field = off ? *field & ~mode->bit : *field | mode->bit;
		return true;
	}

	return false;
}

static const stty_cchar_t *
find_cchar(const char *name)
{
	for (uint32_t index = 0U; index < CCHAR_COUNT; index++) {
		if (nxu_streq(name, g_cchars[index].name)) return &g_cchars[index];
	}

	return 0;
}

/* The operands that take a value: a control character, min, time, a speed, rows, columns. */
typedef enum { VALUED_TAKEN, VALUED_UNKNOWN, VALUED_BAD_VALUE } valued_t;

static valued_t
apply_valued(const char *word, const char *value, struct termios *t, struct winsize *size)
{
	const stty_cchar_t *cchar = find_cchar(word);
	uint64_t number;

	if (cchar != 0 && (cchar->slot == VMIN || cchar->slot == VTIME)) {
		if (!parse_number(value, 10U, 0xFFU, &number)) return VALUED_BAD_VALUE;

		t->c_cc[cchar->slot] = (cc_t)number;
		return VALUED_TAKEN;
	}

	if (cchar != 0) {
		cc_t c;

		if (!parse_cchar(value, &c)) return VALUED_BAD_VALUE;

		t->c_cc[cchar->slot] = c;
		return VALUED_TAKEN;
	}

	if (nxu_streq(word, "speed") || nxu_streq(word, "ispeed") || nxu_streq(word, "ospeed")) {
		if (!parse_number(value, 10U, UINT32_MAX, &number)) return VALUED_BAD_VALUE;

		if (word[0] != 'o') t->c_ispeed = (speed_t)number;
		if (word[0] != 'i') t->c_ospeed = (speed_t)number;

		return VALUED_TAKEN;
	}

	if (nxu_streq(word, "rows")) {
		if (!parse_number(value, 10U, 0xFFFFU, &number)) return VALUED_BAD_VALUE;

		size->ws_row = (unsigned short)number;
		return VALUED_TAKEN;
	}

	if (nxu_streq(word, "columns") || nxu_streq(word, "cols")) {
		if (!parse_number(value, 10U, 0xFFFFU, &number)) return VALUED_BAD_VALUE;

		size->ws_col = (unsigned short)number;
		return VALUED_TAKEN;
	}

	return VALUED_UNKNOWN;
}

/* ---- main --------------------------------------------------------------- */

static int
usage(void)
{
	warn("usage: stty [-a | -g] [-f file] [operands]", 0);
	return 1;
}

int
main(uint64_t arguments, const char *const *argv)
{
	bool all = false;
	bool saved = false;
	const char *file = 0;
	int argc = arguments > 64U ? 64 : (int)arguments;
	int first = 1;

	for (; first < argc && argv[first][0] == '-' && argv[first][1] != '\0'; first++) {
		const char *option = argv[first];

		if (nxu_streq(option, "-a"))
			all = true;
		else if (nxu_streq(option, "-g"))
			saved = true;
		else if (nxu_streq(option, "-e"))
			all = true;
		else if (nxu_streq(option, "-f") && first + 1 < argc)
			file = argv[++first];
		else
			break; /* an operand such as -echo */
	}

	if (all && saved) return usage();

	int descriptor = 0;

	if (file != 0) {
		int64_t opened = nxu_open(file, NXU_O_READ | NXU_O_NONBLOCK);

		if (opened < 0) {
			warn(file, ": cannot open");
			return 1;
		}

		descriptor = (int)opened;
	}

	struct termios termios;
	struct winsize size = {0};

	if (tcgetattr(descriptor, &termios) != 0) {
		warn(file != 0 ? file : "stdin", " isn't a terminal");
		return 1;
	}

	(void)tcgetwinsize(descriptor, &size);

	if (first == argc) {
		if (saved)
			print_saved(&termios);
		else
			print_settings(&termios, &size, all);

		out_flush();
		return 0;
	}

	if (all || saved) return usage();

	struct winsize wanted_size = size;

	for (int index = first; index < argc; index++) {
		const char *word = argv[index];
		const char *value = index + 1 < argc ? argv[index + 1] : 0;
		uint64_t number;

		if (nxu_streq(word, "all") || nxu_streq(word, "everything")) {
			print_settings(&termios, &size, true);
			continue;
		}

		if (nxu_streq(word, "size")) {
			out_decimal(size.ws_row);
			out(" ");
			out_decimal(size.ws_col);
			out("\n");
			continue;
		}

		if (nxu_streq(word, "speed") && value == 0) {
			out_decimal(termios.c_ospeed);
			out("\n");
			continue;
		}

		if (apply_combination(word, &termios) || apply_mode(word, &termios)) continue;

		if (parse_number(word, 10U, UINT32_MAX, &number)) {
			(void)cfsetspeed(&termios, (speed_t)number);
			continue;
		}

		if (parse_saved(word, &termios)) continue;

		if (word[0] == 'g' && word[1] == 'f' && word[2] == 'm' && word[3] == 't') {
			warn("bad saved setting: ", word);
			return 1;
		}

		/* Everything below takes a value. */
		if (value == 0) {
			warn("unknown argument: ", word);
			return 1;
		}

		valued_t result = apply_valued(word, value, &termios, &wanted_size);

		if (result == VALUED_UNKNOWN) {
			warn("unknown argument: ", word);
			return 1;
		}

		if (result == VALUED_BAD_VALUE) {
			warn("invalid value for ", word);
			return 1;
		}

		index++;
	}

	out_flush();

	if (tcsetattr(descriptor, TCSANOW, &termios) != 0) {
		warn("tcsetattr: the terminal refused the settings", 0);
		return 1;
	}

	if ((wanted_size.ws_row != size.ws_row || wanted_size.ws_col != size.ws_col) &&
		tcsetwinsize(descriptor, &wanted_size) != 0) {
		warn("TIOCSWINSZ failed", 0);
		return 1;
	}

	return 0;
}
