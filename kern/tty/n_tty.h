/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/n_tty.h
 *
 * The default line discipline: what a terminal does with the bytes between
 * its driver and its files, as termios configures it.
 *
 * Input. Each byte a driver delivers goes through n_tty_receive, which
 *
 *   - maps it (ISTRIP, IGNCR, ICRNL, INLCR);
 *   - acts on the special characters: output flow control (IXON: VSTOP,
 *     VSTART, IXANY), signals (ISIG: VINTR, VQUIT, VSUSP), and, with IEXTEN,
 *     VLNEXT (take the next byte literally), VDISCARD (toggle FLUSHO),
 *     VWERASE and VREPRINT;
 *   - in canonical mode (ICANON) edits the line being typed (VERASE,
 *     VKILL, VWERASE) and ends it at a newline, VEOL, VEOL2 or VEOF; a read
 *     then returns at most one line, and a line ended by VEOF alone reads as
 *     end of file;
 *   - otherwise stores it, readable at once;
 *   - echoes it (ECHO, ECHOE, ECHOK, ECHOKE, ECHONL, ECHOCTL).
 *
 * Output. n_tty_write and the echo go through the same output processing
 * (OPOST: ONLCR, OCRNL, ONOCR, ONLRET, OXTABS, ONOEOT), which also keeps
 * track of the cursor column, so erasing a tab moves back the right
 * distance. IUTF8 makes an erase remove a whole UTF-8 sequence, which takes
 * one column.
 *
 * Line errors. A driver that sees a break, or a byte with a parity or
 * framing error, hands it over with n_tty_receive_error instead, and termios
 * says what it becomes:
 *
 *   - a break is ignored (IGNBRK); or flushes both ways (unless NOFLSH) and
 *     asks for SIGINT (BRKINT); or reads as \377 \0 \0 (PARMRK) or as \0;
 *   - a byte with a parity or framing error is taken as it is unless INPCK
 *     is set; then it is ignored (IGNPAR), or reads as \377 \0 and the byte
 *     (PARMRK), or as \0;
 *   - while PARMRK marks errors (and IGNPAR does not hide them), a valid \377
 *     reads as \377 \377, so the marks cannot be mistaken for data.
 *
 * A framing error follows INPCK and IGNPAR as a parity error does, as on
 * Linux. An overrun loses bytes before the one it is reported with, and
 * that byte itself is good; the driver counts it and delivers the byte as
 * ordinary input. The marks are stored as literal bytes (never special
 * characters) and echoed like any other input.
 *
 * Not handled here: VSTATUS and
 * VDSUSP (there is no SIGINFO, and a delayed suspend belongs to the read
 * path of job control); ALTWERASE, ECHOPRT, PENDIN and EXTPROC. Their bits are
 * accepted and ignored. VMIN and VTIME are read-time policy and are applied
 * by the terminal core, which can sleep; this file only says how much is
 * available.
 *
 * This file is pure: no locks, no sleeping, no allocation, no kernel
 * headers but the ABI. It works on a buffer the caller owns and reaches the
 * outside world through n_tty_hooks_t, so it compiles natively and is tested
 * on the host under AddressSanitizer (tools/tty). In the kernel every call
 * is made with the terminal's lock held, and every hook runs under it.
 *
 * The input buffer is one ring. Indexes run freely and wrap at 2^32; the
 * ring size is a power of two.
 *
 *     read_tail          canon_head            head
 *         |  complete lines  |  line being edited  |
 *
 * In canonical mode a read takes from read_tail up to the first delimiter
 * (marked in a bitmap, one bit per slot); bytes after canon_head can still
 * be erased. Outside canonical mode canon_head follows head and everything
 * is readable.
 */

#ifndef NXU_KERN_TTY_N_TTY_H
#define NXU_KERN_TTY_N_TTY_H

#include <kern/syscall/termios_defs.h>

#include <stdbool.h>
#include <stdint.h>

/* The signals input can ask for; the caller maps them to its own numbers. */
typedef enum { N_TTY_SIGNAL_INTERRUPT, N_TTY_SIGNAL_QUIT, N_TTY_SIGNAL_SUSPEND } n_tty_signal_t;

/* The line errors a driver can report with a byte (see "Line errors" above). */
typedef enum { N_TTY_ERROR_BREAK, N_TTY_ERROR_PARITY, N_TTY_ERROR_FRAMING } n_tty_error_t;

/*
 * n_tty_hooks_t
 *
 * output_room / output
 *     How many bytes the output queue can take, and appending exactly that
 *     many or fewer. Echo that does not fit is dropped; n_tty_write stops
 *     before a byte whose expansion does not fit.
 *
 * output_flush
 *     Discard everything waiting in the output queue.
 *
 * signal
 *     A special character asks for a signal to the terminal's foreground.
 *
 * flow
 *     Output must stop (true) or may go on (false): IXON.
 */
typedef struct n_tty_hooks {
	uint32_t (*output_room)(void *context);
	void (*output)(void *context, const uint8_t *bytes, uint32_t count);
	void (*output_flush)(void *context);
	void (*signal)(void *context, n_tty_signal_t signal);
	void (*flow)(void *context, bool stopped);
} n_tty_hooks_t;

/*
 * n_tty_t
 *
 * buffer / delimiters / size / mask
 *     The ring (size bytes, a power of two) and its delimiter bitmap
 *     (size / 8 bytes), both the caller's.
 *
 * column / canon_column
 *     Where the cursor is, as output processing has moved it, and where it
 *     was when the first byte of the line being edited was echoed.
 *
 * literal_next
 *     VLNEXT was the last byte: the next one is taken as it is.
 *
 * stopped
 *     Output is stopped by VSTOP.
 *
 * received
 *     Counts bytes stored; the core's VTIME timer restarts when it moves.
 */
typedef struct n_tty {
	struct nxu_termios termios;
	const n_tty_hooks_t *hooks;
	void *context;

	uint8_t *buffer;
	uint8_t *delimiters;
	uint32_t size;
	uint32_t mask;

	uint32_t read_tail;
	uint32_t canon_head;
	uint32_t head;

	uint32_t column;
	uint32_t canon_column;
	bool literal_next;
	bool stopped;
	uint32_t received;
} n_tty_t;

/*
 * n_tty_init
 *
 * An empty discipline over `buffer` (size bytes, a power of two of at least
 * 64) and `delimiters` (size / 8 bytes), configured by `termios`.
 */
void n_tty_init(n_tty_t *ldisc, const n_tty_hooks_t *hooks, void *context, uint8_t *buffer, uint8_t *delimiters,
				uint32_t size, const struct nxu_termios *termios);

/*
 * n_tty_set_termios
 *
 * Reconfigure. Leaving canonical mode makes the line being edited readable;
 * entering it makes everything unread part of the line being edited. Turning
 * IXON off restarts stopped output.
 */
void n_tty_set_termios(n_tty_t *ldisc, const struct nxu_termios *termios);

/*
 * n_tty_receive
 *
 * Process input; returns how many bytes were taken. Canonical mode takes
 * everything (a byte that does not fit in a full buffer is dropped, with a
 * bell under IMAXBEL); otherwise it stops at the first byte to store that
 * does not fit, and the driver offers the rest again when there is room.
 */
uint32_t n_tty_receive(n_tty_t *ldisc, const uint8_t *bytes, uint32_t count);

/*
 * n_tty_receive_error
 *
 * One byte that arrived with a line error (for a break, the byte the
 * hardware read, normally 0). True when it was dealt with; false when what
 * it becomes did not fit (non-canonical mode, buffer full), and the driver
 * offers it again when there is room, as with n_tty_receive.
 */
bool n_tty_receive_error(n_tty_t *ldisc, uint8_t c, n_tty_error_t error);

/* How many bytes n_tty_receive is sure to take: unlimited in canonical mode. */
uint32_t n_tty_receive_room(const n_tty_t *ldisc);

/*
 * n_tty_available / n_tty_line_ready
 *
 * Bytes a non-canonical read could take now, and whether a canonical read
 * would find a whole line (an empty one, for end of file, counts).
 */
uint32_t n_tty_available(const n_tty_t *ldisc);
bool n_tty_line_ready(const n_tty_t *ldisc);

/* Whether a read would find something, by the current mode (VMIN is the core's). */
bool n_tty_readable(const n_tty_t *ldisc);

/*
 * n_tty_read
 *
 * Move input into `buffer`; returns how many bytes. Canonical mode stops
 * after one line's delimiter (VEOF is consumed, not returned, so a line of
 * only VEOF returns 0); a line longer than `capacity` is returned over
 * several reads. Otherwise up to `capacity` of what is available.
 */
uint32_t n_tty_read(n_tty_t *ldisc, uint8_t *buffer, uint32_t capacity);

/*
 * n_tty_write
 *
 * Output processing of what a program writes; returns how many bytes of
 * `bytes` were taken (all of them under FLUSHO, which discards).
 */
uint32_t n_tty_write(n_tty_t *ldisc, const uint8_t *bytes, uint32_t count);

/* Discard all input, complete lines and the line being edited alike. */
void n_tty_flush_input(n_tty_t *ldisc);

#endif
