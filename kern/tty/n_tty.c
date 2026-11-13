/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/n_tty.c
 *
 * The default line discipline. See kern/tty/n_tty.h for what it does, the
 * buffer layout, and the rules for callers; this file is pure code.
 *
 * Delimiters. A canonical line ends at the byte whose bit is set in the
 * delimiter bitmap. A newline, VEOL or VEOL2 is stored as itself and
 * returned by the read; VEOF is stored as NXU_POSIX_VDISABLE, which no
 * enabled control character can equal, and the read consumes it without
 * returning it. Bytes taken literally (VLNEXT) are never delimiters.
 *
 * Room. In canonical mode one slot is kept back for a delimiter, so a line
 * that fills the buffer can still be ended; past that, ordinary bytes are
 * dropped (with a bell under IMAXBEL) rather than refused, because refusing
 * would leave a reader waiting for a line the writer cannot send. Outside
 * canonical mode a full buffer refuses input and the driver holds it back.
 *
 * Columns. One column counter serves output and echo. Erasing a character
 * recomputes where it began by replaying the echo widths of the line from
 * canon_column, which is what lets a tab or a ^X be erased exactly.
 */

#include <kern/tty/n_tty.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define N_TTY_TAB_WIDTH 8U
#define N_TTY_BELL 0x07U

/* ---- configuration ------------------------------------------------------ */

static bool
n_tty_lflag(const n_tty_t *ldisc, nxu_tcflag_t flag)
{
	return (ldisc->termios.c_lflag & flag) != 0U;
}

static bool
n_tty_iflag(const n_tty_t *ldisc, nxu_tcflag_t flag)
{
	return (ldisc->termios.c_iflag & flag) != 0U;
}

/* True when `c` is the control character in `slot` and that slot is enabled. */
static bool
n_tty_is(const n_tty_t *ldisc, uint8_t c, uint32_t slot)
{
	uint8_t value = ldisc->termios.c_cc[slot];

	return value != NXU_POSIX_VDISABLE && c == value;
}

static bool
n_tty_is_control(uint8_t c)
{
	return c < 0x20U || c == 0x7FU;
}

static bool
n_tty_is_continuation(const n_tty_t *ldisc, uint8_t c)
{
	return n_tty_iflag(ldisc, NXU_IUTF8) && (c & 0xC0U) == 0x80U;
}

/* ---- the ring ----------------------------------------------------------- */

static uint8_t
n_tty_at(const n_tty_t *ldisc, uint32_t index)
{
	return ldisc->buffer[index & ldisc->mask];
}

static bool
n_tty_marked(const n_tty_t *ldisc, uint32_t index)
{
	uint32_t slot = index & ldisc->mask;

	return (ldisc->delimiters[slot >> 3U] & (1U << (slot & 7U))) != 0U;
}

static void
n_tty_set_mark(n_tty_t *ldisc, uint32_t index, bool delimiter)
{
	uint32_t slot = index & ldisc->mask;
	uint8_t bit = (uint8_t)(1U << (slot & 7U));

	if (delimiter)
		ldisc->delimiters[slot >> 3U] |= bit;
	else
		ldisc->delimiters[slot >> 3U] &= (uint8_t)~bit;
}

static uint32_t
n_tty_used(const n_tty_t *ldisc)
{
	return ldisc->head - ldisc->read_tail;
}

/*
 * Store one byte. In canonical mode a delimiter ends the line (canon_head
 * moves past it); otherwise everything stored is readable at once.
 */
static bool
n_tty_store(n_tty_t *ldisc, uint8_t c, bool delimiter)
{
	bool canonical = n_tty_lflag(ldisc, NXU_ICANON);
	uint32_t limit = canonical && !delimiter ? ldisc->size - 1U : ldisc->size;

	if (n_tty_used(ldisc) >= limit) return false;

	ldisc->buffer[ldisc->head & ldisc->mask] = c;
	n_tty_set_mark(ldisc, ldisc->head, delimiter);
	ldisc->head++;
	ldisc->received++;

	if (!canonical || delimiter) ldisc->canon_head = ldisc->head;

	return true;
}

/* ---- output ------------------------------------------------------------- */

static uint32_t
n_tty_output_room(const n_tty_t *ldisc)
{
	return ldisc->hooks->output_room != 0 ? ldisc->hooks->output_room(ldisc->context) : 0U;
}

/* Emit bytes as they are, all or nothing. */
static bool
n_tty_emit(n_tty_t *ldisc, const uint8_t *bytes, uint32_t count)
{
	if (count == 0U) return true;
	if (ldisc->hooks->output == 0 || n_tty_output_room(ldisc) < count) return false;

	ldisc->hooks->output(ldisc->context, bytes, count);
	return true;
}

static uint32_t
n_tty_tab_stop(uint32_t column)
{
	return N_TTY_TAB_WIDTH - (column % N_TTY_TAB_WIDTH);
}

/*
 * Output one byte with output processing; false, with nothing written and
 * no state changed, when the queue has no room for what it expands to.
 */
static bool
n_tty_output_byte(n_tty_t *ldisc, uint8_t c)
{
	nxu_tcflag_t oflag = ldisc->termios.c_oflag;
	bool post = (oflag & NXU_OPOST) != 0U;
	uint8_t expanded[N_TTY_TAB_WIDTH];
	uint32_t length = 1U;
	uint32_t column = ldisc->column;

	expanded[0] = c;

	switch (c) {
	case '\n':
		if (post && (oflag & NXU_ONLCR) != 0U) {
			expanded[0] = '\r';
			expanded[1] = '\n';
			length = 2U;
			column = 0U;
		} else if (post && (oflag & NXU_ONLRET) != 0U) {
			column = 0U;
		}

		break;

	case '\r':
		if (post && (oflag & NXU_ONOCR) != 0U && column == 0U) return true;

		if (post && (oflag & NXU_OCRNL) != 0U) {
			expanded[0] = '\n';
			if ((oflag & NXU_ONLRET) != 0U) column = 0U;
		} else {
			column = 0U;
		}

		break;

	case '\t': {
		uint32_t spaces = n_tty_tab_stop(column);

		if (post && (oflag & NXU_OXTABS) != 0U) {
			memset(expanded, ' ', spaces);
			length = spaces;
		}

		column += spaces;
		break;
	}

	case '\b':
		if (column != 0U) column--;
		break;

	case 0x04U:
		if (post && (oflag & NXU_ONOEOT) != 0U) return true;
		break;

	default:
		if (!n_tty_is_control(c) && !n_tty_is_continuation(ldisc, c)) column++;
		break;
	}

	if (!n_tty_emit(ldisc, expanded, length)) return false;

	ldisc->column = column;
	return true;
}

/* ---- echo --------------------------------------------------------------- */

/* How many columns echoing `c` at `column` takes. */
static uint32_t
n_tty_echo_width(const n_tty_t *ldisc, uint8_t c, uint32_t column)
{
	if (c == '\t') return n_tty_tab_stop(column);
	if (n_tty_is_control(c)) return n_tty_lflag(ldisc, NXU_ECHOCTL) ? 2U : 0U;
	if (n_tty_is_continuation(ldisc, c)) return 0U;

	return 1U;
}

/* Echo one input byte (the caller has checked ECHO). Echo that does not fit is dropped. */
static void
n_tty_echo(n_tty_t *ldisc, uint8_t c)
{
	if (n_tty_lflag(ldisc, NXU_ECHOCTL) && n_tty_is_control(c) && c != '\t' && c != '\n') {
		uint8_t caret[2] = {'^', (uint8_t)(c ^ 0x40U)};

		if (n_tty_emit(ldisc, caret, 2U)) ldisc->column += 2U;

		return;
	}

	(void)n_tty_output_byte(ldisc, c);
}

static void
n_tty_echo_if(n_tty_t *ldisc, uint8_t c)
{
	if (n_tty_lflag(ldisc, NXU_ECHO)) n_tty_echo(ldisc, c);
}

/* Move the cursor back over `width` columns, wiping them unless they are a tab's. */
static void
n_tty_echo_rubout(n_tty_t *ldisc, uint32_t width, bool wipe)
{
	static const uint8_t wiper[3] = {'\b', ' ', '\b'};
	static const uint8_t back = '\b';

	for (uint32_t step = 0U; step < width; step++) {
		if (!(wipe ? n_tty_emit(ldisc, wiper, 3U) : n_tty_emit(ldisc, &back, 1U))) return;

		if (ldisc->column != 0U) ldisc->column--;
	}
}

/* ---- editing ------------------------------------------------------------ */

/*
 * Remove the last character of the line being edited (a whole UTF-8
 * sequence under IUTF8). With `visual`, and echo on, rub it out on the
 * screen as well. False when the line is empty.
 */
static bool
n_tty_erase_one(n_tty_t *ldisc, bool visual)
{
	if (ldisc->head == ldisc->canon_head) return false;

	uint32_t end = ldisc->head;
	uint32_t start = end - 1U;

	if (n_tty_iflag(ldisc, NXU_IUTF8)) {
		while (start != ldisc->canon_head && n_tty_is_continuation(ldisc, n_tty_at(ldisc, start)))
			start--;
	}

	if (visual && n_tty_lflag(ldisc, NXU_ECHO)) {
		uint32_t column = ldisc->canon_column;

		for (uint32_t index = ldisc->canon_head; index != start; index++)
			column += n_tty_echo_width(ldisc, n_tty_at(ldisc, index), column);

		uint32_t width = 0U;

		for (uint32_t index = start; index != end; index++)
			width += n_tty_echo_width(ldisc, n_tty_at(ldisc, index), column + width);

		n_tty_echo_rubout(ldisc, width, n_tty_at(ldisc, start) != '\t');
	}

	ldisc->head = start;
	return true;
}

static bool
n_tty_is_blank(uint8_t c)
{
	return c == ' ' || c == '\t';
}

/* VERASE: one character; echoed as a rub-out under ECHOE, as itself otherwise. */
static void
n_tty_erase(n_tty_t *ldisc, uint8_t c)
{
	bool visual = n_tty_lflag(ldisc, NXU_ECHOE);

	if (n_tty_erase_one(ldisc, visual) && !visual) n_tty_echo_if(ldisc, c);
}

/* VWERASE: blanks before the cursor, then the word before them. */
static void
n_tty_erase_word(n_tty_t *ldisc, uint8_t c)
{
	bool visual = n_tty_lflag(ldisc, NXU_ECHOE);
	bool erased = false;

	while (ldisc->head != ldisc->canon_head && n_tty_is_blank(n_tty_at(ldisc, ldisc->head - 1U)))
		erased |= n_tty_erase_one(ldisc, visual);

	while (ldisc->head != ldisc->canon_head && !n_tty_is_blank(n_tty_at(ldisc, ldisc->head - 1U)))
		erased |= n_tty_erase_one(ldisc, visual);

	if (erased && !visual) n_tty_echo_if(ldisc, c);
}

/* VKILL: the whole line. ECHOKE rubs it out; otherwise the kill character is echoed, then a newline under ECHOK. */
static void
n_tty_kill(n_tty_t *ldisc, uint8_t c)
{
	if (ldisc->head == ldisc->canon_head) return;

	if (n_tty_lflag(ldisc, NXU_ECHOKE)) {
		while (n_tty_erase_one(ldisc, true)) {
		}

		return;
	}

	ldisc->head = ldisc->canon_head;

	if (!n_tty_lflag(ldisc, NXU_ECHO)) return;

	n_tty_echo(ldisc, c);
	if (n_tty_lflag(ldisc, NXU_ECHOK)) (void)n_tty_output_byte(ldisc, '\n');
}

/* VREPRINT: echo it, a newline, then the line being edited again. */
static void
n_tty_reprint(n_tty_t *ldisc, uint8_t c)
{
	if (!n_tty_lflag(ldisc, NXU_ECHO)) return;

	n_tty_echo(ldisc, c);
	(void)n_tty_output_byte(ldisc, '\n');
	ldisc->canon_column = ldisc->column;

	for (uint32_t index = ldisc->canon_head; index != ldisc->head; index++)
		n_tty_echo(ldisc, n_tty_at(ldisc, index));
}

/* ---- special characters ------------------------------------------------- */

static void
n_tty_set_stopped(n_tty_t *ldisc, bool stopped)
{
	if (ldisc->stopped == stopped) return;

	ldisc->stopped = stopped;
	if (ldisc->hooks->flow != 0) ldisc->hooks->flow(ldisc->context, stopped);
}

/* VINTR, VQUIT, VSUSP: flush both ways unless NOFLSH, echo, and ask for the signal. */
static void
n_tty_signal_char(n_tty_t *ldisc, uint8_t c, n_tty_signal_t signal)
{
	if (!n_tty_lflag(ldisc, NXU_NOFLSH)) {
		n_tty_flush_input(ldisc);
		if (ldisc->hooks->output_flush != 0) ldisc->hooks->output_flush(ldisc->context);
	}

	n_tty_set_stopped(ldisc, false);
	n_tty_echo_if(ldisc, c);

	if (ldisc->hooks->signal != 0) ldisc->hooks->signal(ldisc->context, signal);
}

/* VDISCARD: start discarding output, or stop. */
static void
n_tty_discard(n_tty_t *ldisc, uint8_t c)
{
	if (n_tty_lflag(ldisc, NXU_FLUSHO)) {
		ldisc->termios.c_lflag &= ~NXU_FLUSHO;
		return;
	}

	if (ldisc->hooks->output_flush != 0) ldisc->hooks->output_flush(ldisc->context);

	n_tty_echo_if(ldisc, c);
	ldisc->termios.c_lflag |= NXU_FLUSHO;
}

/*
 * The flow-control and signal characters, which act in every mode; true
 * when `c` was one of them and has been dealt with.
 */
static bool
n_tty_receive_control(n_tty_t *ldisc, uint8_t c)
{
	if (n_tty_iflag(ldisc, NXU_IXON)) {
		bool start = n_tty_is(ldisc, c, NXU_VSTART);
		bool stop = n_tty_is(ldisc, c, NXU_VSTOP);

		if (start && stop) {
			n_tty_set_stopped(ldisc, !ldisc->stopped);
			return true;
		}

		if (stop) {
			n_tty_set_stopped(ldisc, true);
			return true;
		}

		if (start) {
			n_tty_set_stopped(ldisc, false);
			return true;
		}

		if (ldisc->stopped && n_tty_iflag(ldisc, NXU_IXANY)) n_tty_set_stopped(ldisc, false);
	}

	if (n_tty_lflag(ldisc, NXU_ISIG)) {
		if (n_tty_is(ldisc, c, NXU_VINTR)) {
			n_tty_signal_char(ldisc, c, N_TTY_SIGNAL_INTERRUPT);
			return true;
		}

		if (n_tty_is(ldisc, c, NXU_VQUIT)) {
			n_tty_signal_char(ldisc, c, N_TTY_SIGNAL_QUIT);
			return true;
		}

		if (n_tty_is(ldisc, c, NXU_VSUSP)) {
			n_tty_signal_char(ldisc, c, N_TTY_SIGNAL_SUSPEND);
			return true;
		}
	}

	if (n_tty_lflag(ldisc, NXU_IEXTEN)) {
		if (n_tty_is(ldisc, c, NXU_VLNEXT)) {
			static const uint8_t hint[2] = {'^', '\b'};

			/* "^" under the cursor until the literal byte replaces it. */
			if (n_tty_lflag(ldisc, NXU_ECHO) && n_tty_lflag(ldisc, NXU_ECHOCTL)) (void)n_tty_emit(ldisc, hint, 2U);

			ldisc->literal_next = true;
			return true;
		}

		if (n_tty_is(ldisc, c, NXU_VDISCARD)) {
			n_tty_discard(ldisc, c);
			return true;
		}
	}

	return false;
}

/* ---- input -------------------------------------------------------------- */

/*
 * Store an ordinary byte (not a delimiter) and echo it. False only when it
 * was refused (non-canonical mode, buffer full); in canonical mode a byte
 * that does not fit is dropped and true returned.
 */
static bool
n_tty_receive_ordinary(n_tty_t *ldisc, uint8_t c)
{
	bool canonical = n_tty_lflag(ldisc, NXU_ICANON);

	/* The first byte of a line: erasing never goes left of where it is echoed. */
	if (canonical && ldisc->head == ldisc->canon_head) ldisc->canon_column = ldisc->column;

	if (!n_tty_store(ldisc, c, false)) {
		if (!canonical) return false;

		if (n_tty_iflag(ldisc, NXU_IMAXBEL)) {
			static const uint8_t bell = N_TTY_BELL;

			(void)n_tty_emit(ldisc, &bell, 1U);
		}

		return true;
	}

	n_tty_echo_if(ldisc, c);
	return true;
}

/* A line ends: store the delimiter and echo it (a newline also under ECHONL). */
static void
n_tty_receive_delimiter(n_tty_t *ldisc, uint8_t c)
{
	/* A slot is always kept for this, so it cannot fail. */
	(void)n_tty_store(ldisc, c, true);

	if (n_tty_lflag(ldisc, NXU_ECHO))
		n_tty_echo(ldisc, c);
	else if (c == '\n' && n_tty_lflag(ldisc, NXU_ECHONL))
		(void)n_tty_output_byte(ldisc, '\n');
}

/* One byte; false when it was refused and must be offered again. */
static bool
n_tty_receive_byte(n_tty_t *ldisc, uint8_t c)
{
	if (n_tty_iflag(ldisc, NXU_ISTRIP)) c &= 0x7FU;

	if (ldisc->literal_next) {
		if (!n_tty_receive_ordinary(ldisc, c)) return false;

		ldisc->literal_next = false;
		return true;
	}

	if (n_tty_receive_control(ldisc, c)) return true;

	if (c == '\r') {
		if (n_tty_iflag(ldisc, NXU_IGNCR)) return true;
		if (n_tty_iflag(ldisc, NXU_ICRNL)) c = '\n';
	} else if (c == '\n' && n_tty_iflag(ldisc, NXU_INLCR)) {
		c = '\r';
	}

	if (!n_tty_lflag(ldisc, NXU_ICANON)) return n_tty_receive_ordinary(ldisc, c);

	if (n_tty_is(ldisc, c, NXU_VERASE)) {
		n_tty_erase(ldisc, c);
		return true;
	}

	if (n_tty_is(ldisc, c, NXU_VKILL)) {
		n_tty_kill(ldisc, c);
		return true;
	}

	if (n_tty_lflag(ldisc, NXU_IEXTEN) && n_tty_is(ldisc, c, NXU_VWERASE)) {
		n_tty_erase_word(ldisc, c);
		return true;
	}

	if (n_tty_lflag(ldisc, NXU_IEXTEN) && n_tty_is(ldisc, c, NXU_VREPRINT)) {
		n_tty_reprint(ldisc, c);
		return true;
	}

	if (n_tty_is(ldisc, c, NXU_VEOF)) {
		(void)n_tty_store(ldisc, NXU_POSIX_VDISABLE, true);
		return true;
	}

	if (c == '\n' || n_tty_is(ldisc, c, NXU_VEOL) || n_tty_is(ldisc, c, NXU_VEOL2)) {
		n_tty_receive_delimiter(ldisc, c);
		return true;
	}

	return n_tty_receive_ordinary(ldisc, c);
}

/* ---- the interface ------------------------------------------------------ */

void
n_tty_init(n_tty_t *ldisc, const n_tty_hooks_t *hooks, void *context, uint8_t *buffer, uint8_t *delimiters,
		   uint32_t size, const struct nxu_termios *termios)
{
	memset(ldisc, 0, sizeof(*ldisc));
	ldisc->hooks = hooks;
	ldisc->context = context;
	ldisc->buffer = buffer;
	ldisc->delimiters = delimiters;
	ldisc->size = size;
	ldisc->mask = size - 1U;
	ldisc->termios = *termios;

	memset(delimiters, 0, size / 8U);
}

void
n_tty_set_termios(n_tty_t *ldisc, const struct nxu_termios *termios)
{
	bool was_canonical = n_tty_lflag(ldisc, NXU_ICANON);

	ldisc->termios = *termios;

	bool canonical = n_tty_lflag(ldisc, NXU_ICANON);

	if (was_canonical && !canonical) {
		/* The line being edited is input like any other now. */
		ldisc->canon_head = ldisc->head;
	} else if (!was_canonical && canonical) {
		/* Everything unread becomes the line being edited. */
		for (uint32_t index = ldisc->read_tail; index != ldisc->head; index++)
			n_tty_set_mark(ldisc, index, false);

		ldisc->canon_head = ldisc->read_tail;
		ldisc->canon_column = ldisc->column;
	}

	if (!n_tty_iflag(ldisc, NXU_IXON)) n_tty_set_stopped(ldisc, false);
	if (!n_tty_lflag(ldisc, NXU_IEXTEN)) ldisc->literal_next = false;
}

uint32_t
n_tty_receive(n_tty_t *ldisc, const uint8_t *bytes, uint32_t count)
{
	uint32_t done = 0U;

	while (done < count && n_tty_receive_byte(ldisc, bytes[done]))
		done++;

	return done;
}

uint32_t
n_tty_receive_room(const n_tty_t *ldisc)
{
	if (n_tty_lflag(ldisc, NXU_ICANON)) return UINT32_MAX;

	return ldisc->size - n_tty_used(ldisc);
}

uint32_t
n_tty_available(const n_tty_t *ldisc)
{
	if (n_tty_lflag(ldisc, NXU_ICANON)) return ldisc->canon_head - ldisc->read_tail;

	return n_tty_used(ldisc);
}

bool
n_tty_line_ready(const n_tty_t *ldisc)
{
	return ldisc->canon_head != ldisc->read_tail;
}

bool
n_tty_readable(const n_tty_t *ldisc)
{
	return n_tty_lflag(ldisc, NXU_ICANON) ? n_tty_line_ready(ldisc) : n_tty_used(ldisc) != 0U;
}

uint32_t
n_tty_read(n_tty_t *ldisc, uint8_t *buffer, uint32_t capacity)
{
	uint32_t count = 0U;

	if (!n_tty_lflag(ldisc, NXU_ICANON)) {
		while (count < capacity && ldisc->read_tail != ldisc->head) {
			n_tty_set_mark(ldisc, ldisc->read_tail, false);
			buffer[count++] = n_tty_at(ldisc, ldisc->read_tail);
			ldisc->read_tail++;
		}

		return count;
	}

	while (count < capacity && ldisc->read_tail != ldisc->canon_head) {
		uint32_t index = ldisc->read_tail;
		uint8_t c = n_tty_at(ldisc, index);
		bool delimiter = n_tty_marked(ldisc, index);

		ldisc->read_tail++;

		if (delimiter) {
			n_tty_set_mark(ldisc, index, false);
			if (c != NXU_POSIX_VDISABLE) buffer[count++] = c;

			return count;
		}

		buffer[count++] = c;
	}

	/*
	 * Stopped by `capacity` just before a VEOF that ends this line: take it
	 * now, or the next read would find it alone and report end of file for
	 * a line that had text.
	 */
	if (count != 0U && ldisc->read_tail != ldisc->canon_head && n_tty_marked(ldisc, ldisc->read_tail) &&
		n_tty_at(ldisc, ldisc->read_tail) == NXU_POSIX_VDISABLE) {
		n_tty_set_mark(ldisc, ldisc->read_tail, false);
		ldisc->read_tail++;
	}

	return count;
}

uint32_t
n_tty_write(n_tty_t *ldisc, const uint8_t *bytes, uint32_t count)
{
	if (n_tty_lflag(ldisc, NXU_FLUSHO)) return count;

	uint32_t done = 0U;

	while (done < count && n_tty_output_byte(ldisc, bytes[done]))
		done++;

	return done;
}

void
n_tty_flush_input(n_tty_t *ldisc)
{
	ldisc->read_tail = ldisc->head;
	ldisc->canon_head = ldisc->head;
	ldisc->literal_next = false;
	memset(ldisc->delimiters, 0, ldisc->size / 8U);
}
