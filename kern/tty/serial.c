/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/serial.c
 *
 * The serial terminal, /dev/console. See kern/tty/serial.h for what it is.
 *
 * There is one line, so one static struct serial. It is the terminal's
 * driver context, and the terminal is never released: the driver's
 * reference (tty_create's) is held for the life of the kernel, and the
 * release operation is never called.
 *
 * Receiving. Input goes through three places:
 *
 *     UART FIFO --(interrupt)--> ring --(receive thread)--> terminal
 *
 * The interrupt handler (serial_interrupt) moves what the UART holds into a
 * ring of bytes and their UART_RX_* status, and wakes the receive thread,
 * which hands the ring's contents to the terminal: plain bytes with
 * tty_input, a break or a byte with an error with tty_input_error. The
 * interrupt never touches the terminal, which runs in thread context only.
 *
 * Holding input back. When the ring is full the handler turns the receive
 * interrupt off and leaves the rest in the UART, and the thread turns it on
 * again once the ring is half empty, draining the UART itself as it does
 * (what waits there may raise no new interrupt). When the terminal refuses input (raw
 * mode, nobody reading), the thread stops offering it until the terminal's
 * unthrottle operation says there is room, and the ring fills. So a full
 * terminal backs up into the UART, and a sender that waits for room in the
 * UART (QEMU's character devices) waits rather than losing anything; on real
 * hardware the UART's own FIFO overflows, and the overrun is counted.
 *
 * Without a receive interrupt the thread itself drains the UART into the
 * ring every SERIAL_POLL_US, and simply stops reading it when the ring is
 * full.
 *
 * Transmitting is polled: the start operation takes the terminal's output
 * and writes it with kconsole_serial_write, in short chunks, each under the
 * console lock (interrupts masked). One caller at a time moves output; one
 * that finds a move in progress asks the mover to go round once more (the
 * pattern of kern/tty/pty.c), so chunks from two writers never interleave.
 *
 * Locking: s_lock, irqsave, covers the ring and every field below it in
 * struct serial; the interrupt handler takes it. It is never held across a
 * call into kern/tty/tty.c or the console, and it is the only lock around
 * uart_get_byte and uart_receive_interrupt_enable. Lock order: the terminal's
 * lock is never held when s_lock is taken, and the reverse never happens
 * either (s_lock is always dropped before calling the terminal).
 */

#include <kern/tty/serial.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/syscall/termios_defs.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Received bytes waiting for the receive thread; a power of two. */
#define SERIAL_RING_SIZE 1024U

/* What the thread hands the terminal in one go, and what start writes under the console lock in one go. */
#define SERIAL_DELIVER_CHUNK 128U
#define SERIAL_OUTPUT_CHUNK 64U

/* How often a line with no receive interrupt is polled. */
#define SERIAL_POLL_US 10000ULL

/* The name in /dev; root's alone, as a console is until a login hands it over. */
#define SERIAL_NAME "console"
#define SERIAL_MODE 0600U

/* The errors that make a byte something other than plain input. */
#define SERIAL_RX_ERRORS (UART_RX_BREAK | UART_RX_PARITY | UART_RX_FRAMING)

/*
 * struct serial
 *
 * s_tty is fixed once serial_init has made it. Everything from s_lock on
 * is protected by s_lock, except the wait queue, which has its own lock.
 *
 * s_bytes / s_status / s_head / s_count
 *     The ring: received bytes and their UART_RX_* status, oldest at s_head.
 *
 * s_interrupt / s_receiving
 *     The line is interrupt driven; its receive interrupt is on right now.
 *
 * s_refused / s_unthrottles
 *     The terminal refused input, and the thread waits for its unthrottle
 *     operation before offering more; how many unthrottles have happened,
 *     so one that comes between a refusal and the thread recording it is
 *     not lost.
 *
 * s_moving / s_again
 *     A start is moving output; another caller asked it to look again.
 *
 * s_line
 *     The line settings last made, so an unchanged termios costs nothing.
 */
struct serial {
	tty_t s_tty;

	nxu_spinlock_t s_lock;
	uint8_t s_bytes[SERIAL_RING_SIZE];
	uint8_t s_status[SERIAL_RING_SIZE];
	uint32_t s_head;
	uint32_t s_count;
	bool s_interrupt;
	bool s_receiving;
	bool s_refused;
	uint32_t s_unthrottles;
	bool s_moving;
	bool s_again;
	uart_line_t s_line;
	serial_stats_t s_stats;

	waitq_t s_wait; /* the receive thread, waiting for input or for room */
};

static struct serial g_serial = {.s_lock = NXU_SPINLOCK_INIT};

static void serial_start(tty_t tty, void *context);
static void serial_unthrottle(tty_t tty, void *context);
static vfs_status_t serial_param(tty_t tty, void *context, const struct nxu_termios *termios);

static const tty_driver_operations_t g_serial_driver = {
	.start = serial_start, .unthrottle = serial_unthrottle, .param = serial_param};

static uint64_t
serial_lock(struct serial *serial)
{
	return nxu_spin_lock_irqsave(&serial->s_lock);
}

static void
serial_unlock(struct serial *serial, uint64_t state)
{
	nxu_spin_unlock_irqrestore(&serial->s_lock, state);
}

/* ---- receiving ---------------------------------------------------------- */

/*
 * Move what the UART holds into the ring, as much as fits; s_lock held.
 * True when anything was moved. An interrupt-driven line whose ring is now
 * full has its receive interrupt turned off, leaving the rest in the UART.
 */
static bool
serial_drain_hardware_locked(struct serial *serial)
{
	bool moved = false;

	while (serial->s_count < SERIAL_RING_SIZE) {
		uint8_t byte;
		uint32_t status;

		if (!uart_get_byte(&byte, &status)) break;

		uint32_t slot = (serial->s_head + serial->s_count) & (SERIAL_RING_SIZE - 1U);

		serial->s_bytes[slot] = byte;
		serial->s_status[slot] = (uint8_t)status;
		serial->s_count++;
		serial->s_stats.received++;
		moved = true;
	}

	if (serial->s_count == SERIAL_RING_SIZE && serial->s_interrupt && serial->s_receiving) {
		uart_receive_interrupt_enable(false);
		serial->s_receiving = false;
		serial->s_stats.held++;
	}

	return moved;
}

/* The UART's receive interrupt, in interrupt context. */
static void
serial_interrupt(void *context)
{
	struct serial *serial = context;
	uint64_t state = serial_lock(serial);

	serial->s_stats.interrupts++;

	bool moved = serial_drain_hardware_locked(serial);

	serial_unlock(serial, state);

	if (moved) waitq_wake_all(&serial->s_wait);
}

/*
 * Hand `count` received bytes to the terminal, in order; returns how many it
 * took. Runs of plain bytes go through tty_input together; a break or a
 * byte with an error goes alone through tty_input_error.
 */
static uint32_t
serial_deliver(struct serial *serial, const uint8_t *bytes, const uint8_t *status, uint32_t count)
{
	uint32_t done = 0U;

	while (done < count) {
		if ((status[done] & SERIAL_RX_ERRORS) != 0U) {
			tty_input_error_t error = (status[done] & UART_RX_BREAK) != 0U	  ? TTY_INPUT_BREAK
									  : (status[done] & UART_RX_PARITY) != 0U ? TTY_INPUT_PARITY
																			  : TTY_INPUT_FRAMING;

			if (!tty_input_error(serial->s_tty, bytes[done], error)) break;

			done++;
			continue;
		}

		/* The run of plain bytes starting here; an overrun's byte is one of them. */
		uint32_t end = done + 1U;

		while (end < count && (status[end] & SERIAL_RX_ERRORS) == 0U)
			end++;

		uint32_t run = end - done;
		uint32_t taken = tty_input(serial->s_tty, bytes + done, run);

		done += taken;
		if (taken < run) break;
	}

	return done;
}

/* Count what the delivered bytes' status reported; s_lock held. */
static void
serial_count_locked(struct serial *serial, const uint8_t *status, uint32_t count)
{
	for (uint32_t index = 0U; index < count; index++) {
		uint8_t flags = status[index];

		if ((flags & UART_RX_OVERRUN) != 0U) serial->s_stats.overruns++;

		if ((flags & UART_RX_BREAK) != 0U)
			serial->s_stats.breaks++;
		else if ((flags & UART_RX_PARITY) != 0U)
			serial->s_stats.parity++;
		else if ((flags & UART_RX_FRAMING) != 0U)
			serial->s_stats.framing++;
	}
}

/*
 * serial_receive_thread
 *
 * Hand the ring to the terminal, for ever. The sequence number is taken
 * before the ring and the refusal are looked at; the interrupt handler and
 * the unthrottle operation change those first and wake after, so no wakeup
 * is lost (kern/sched_prism/waitq.h).
 */
static void
serial_receive_thread(void *context)
{
	struct serial *serial = context;
	uint8_t bytes[SERIAL_DELIVER_CHUNK];
	uint8_t status[SERIAL_DELIVER_CHUNK];

	for (;;) {
		uint32_t seq = waitq_seq(&serial->s_wait);
		uint64_t state = serial_lock(serial);

		if (!serial->s_interrupt) (void)serial_drain_hardware_locked(serial);

		uint32_t count = 0U;

		if (!serial->s_refused) {
			count = serial->s_count < SERIAL_DELIVER_CHUNK ? serial->s_count : SERIAL_DELIVER_CHUNK;

			/* Copied, not taken: only what the terminal accepts leaves the ring. Only this thread removes. */
			for (uint32_t index = 0U; index < count; index++) {
				uint32_t slot = (serial->s_head + index) & (SERIAL_RING_SIZE - 1U);

				bytes[index] = serial->s_bytes[slot];
				status[index] = serial->s_status[slot];
			}
		}

		uint32_t unthrottles = serial->s_unthrottles;

		serial_unlock(serial, state);

		if (count == 0U) {
			if (serial->s_interrupt) {
				(void)waitq_block_seq(&serial->s_wait, seq, false);
			} else {
				(void)waitq_block_deadline(&serial->s_wait, seq, false, timer_get_microseconds() + SERIAL_POLL_US);
			}

			continue;
		}

		uint32_t taken = serial_deliver(serial, bytes, status, count);

		state = serial_lock(serial);

		serial->s_head = (serial->s_head + taken) & (SERIAL_RING_SIZE - 1U);
		serial->s_count -= taken;
		serial->s_stats.delivered += taken;
		serial_count_locked(serial, status, taken);

		/* Refused, and no unthrottle since we looked: wait for one. */
		if (taken < count && serial->s_unthrottles == unthrottles) serial->s_refused = true;

		/*
		 * Room again: let the UART interrupt, and take what it kept now. That
		 * may raise no interrupt of its own: a PL011 raises one as bytes
		 * arrive, not while they wait, and a sender that waits for room sends
		 * nothing more into a full FIFO. The loop delivers what was taken.
		 */
		if (serial->s_interrupt && !serial->s_receiving && serial->s_count <= SERIAL_RING_SIZE / 2U) {
			serial->s_receiving = true;
			uart_receive_interrupt_enable(true);
			(void)serial_drain_hardware_locked(serial);
		}

		serial_unlock(serial, state);
	}
}

/* The terminal has room for input again. */
static void
serial_unthrottle(tty_t tty, void *context)
{
	(void)tty;

	struct serial *serial = context;
	uint64_t state = serial_lock(serial);

	serial->s_refused = false;
	serial->s_unthrottles++;

	serial_unlock(serial, state);

	waitq_wake_all(&serial->s_wait);
}

/* ---- transmitting ------------------------------------------------------- */

static void
serial_start(tty_t tty, void *context)
{
	struct serial *serial = context;
	uint8_t chunk[SERIAL_OUTPUT_CHUNK];
	uint64_t state = serial_lock(serial);

	if (serial->s_moving) {
		serial->s_again = true;
		serial_unlock(serial, state);
		return;
	}

	serial->s_moving = true;

	do {
		serial->s_again = false;
		serial_unlock(serial, state);

		for (;;) {
			uint32_t count = tty_output_take(tty, chunk, sizeof(chunk));

			if (count == 0U) break;

			kconsole_serial_write(chunk, count);
		}

		state = serial_lock(serial);
	} while (serial->s_again);

	serial->s_moving = false;
	serial_unlock(serial, state);
}

/* ---- the line ----------------------------------------------------------- */

/*
 * serial_param
 *
 * The line settings termios asks for: the output speed (an input speed of
 * its own is refused; 0 asks for a hangup there are no modem lines to make,
 * and keeps the line as it is), the character size, the stop bits and the
 * parity. What the UART cannot do is refused.
 */
static vfs_status_t
serial_param(tty_t tty, void *context, const struct nxu_termios *termios)
{
	(void)tty;

	struct serial *serial = context;

	if (termios->c_ispeed != 0U && termios->c_ispeed != termios->c_ospeed) return VFS_STATUS_INVALID;
	if (termios->c_ospeed == NXU_B0) return VFS_STATUS_OK;

	static const uint32_t sizes[4] = {5U, 6U, 7U, 8U};
	uart_line_t line = {
		.speed = termios->c_ospeed,
		.data_bits = sizes[(termios->c_cflag & NXU_CSIZE) >> 8U],
		.stop_bits = (termios->c_cflag & NXU_CSTOPB) != 0U ? 2U : 1U,
		.parity = (termios->c_cflag & NXU_PARENB) == 0U	 ? UART_PARITY_NONE
				  : (termios->c_cflag & NXU_PARODD) != 0U ? UART_PARITY_ODD
														  : UART_PARITY_EVEN,
	};

	uint64_t state = serial_lock(serial);
	bool same = memcmp(&line, &serial->s_line, sizeof(line)) == 0;

	serial_unlock(serial, state);

	if (same) return VFS_STATUS_OK;
	if (!kconsole_serial_set_line(&line)) return VFS_STATUS_INVALID;

	state = serial_lock(serial);
	serial->s_line = line;
	serial_unlock(serial, state);

	return VFS_STATUS_OK;
}

/* ---- the interface ------------------------------------------------------ */

bool
serial_init(void)
{
	struct serial *serial = &g_serial;

	kprintf("initializing serial\n");

	if (serial->s_tty != TTY_NULL) return true;

	waitq_init(&serial->s_wait);

	/* 115200 8N1, what the kernel log has used from the start, now with the FIFOs known to be on. */
	serial->s_line = (uart_line_t){.speed = 115200U, .data_bits = 8U, .stop_bits = 1U, .parity = UART_PARITY_NONE};
	if (!kconsole_serial_set_line(&serial->s_line)) kprintf("the UART kept its own line settings\n");

	tty_t tty;

	if (!tty_create(&g_serial_driver, serial, &tty)) {
		kprintf("cannot make the terminal\n");
		return false;
	}

	/* A line with no modem control: CLOCAL, and the speed the line really runs at. */
	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	termios.c_cflag |= NXU_CLOCAL;
	termios.c_ispeed = NXU_B115200;
	termios.c_ospeed = NXU_B115200;

	if (tty_set_termios(tty, &termios) != VFS_STATUS_OK) {
		kprintf("cannot configure the terminal\n");
		tty_detach(tty);
		tty_release(tty);
		return false;
	}

	if (!tty_register(tty, SERIAL_NAME, 0U, 0U, SERIAL_MODE)) {
		kprintf("cannot publish /dev/%s\n", SERIAL_NAME);
		tty_detach(tty);
		tty_release(tty);
		return false;
	}

	serial->s_tty = tty;

	thread_t thread = 0;

	if (!kernel_thread_create(proc_task(proc_kernel()), serial_receive_thread, serial, &thread) ||
		!sched_thread_start(thread)) {
		kprintf("cannot start the receive thread\n");
		if (thread != 0) {
			(void)thread_terminate(thread);
			(void)thread_reap(thread);
			thread_deallocate(thread);
		}

		/* The name stays and output works; input is never read. */
		return false;
	}

	thread_deallocate(thread);

	/*
	 * Interrupt driven if the UART has an interrupt we can take. It is
	 * attached with the UART's receive interrupt still off, and turned on
	 * only once the line is marked interrupt driven, so the first one finds
	 * it so and the thread stops polling.
	 */
	bool interrupt = uart_receive_interrupt_attach(serial_interrupt, serial);
	uint64_t state = serial_lock(serial);

	serial->s_interrupt = interrupt;
	serial->s_stats.interrupt = interrupt;

	if (interrupt) {
		serial->s_receiving = true;
		uart_receive_interrupt_enable(true);
	}

	serial_unlock(serial, state);

	/* Polling until now: wake the thread so it sleeps the right way. */
	waitq_wake_all(&serial->s_wait);

	kprintf("/dev/%s on the console UART, %s\n", SERIAL_NAME,
			serial->s_interrupt ? "interrupt driven" : "polled every 10 ms");
	kprintf("initialized serial successfully\n");
	return true;
}

bool
serial_stats(serial_stats_t *stats)
{
	struct serial *serial = &g_serial;

	if (stats == 0 || serial->s_tty == TTY_NULL) return false;

	uint64_t state = serial_lock(serial);

	*stats = serial->s_stats;
	serial_unlock(serial, state);

	return true;
}

tty_t
serial_tty(void)
{
	tty_t tty = g_serial.s_tty;

	if (tty == TTY_NULL || !tty_reference(tty)) return TTY_NULL;

	return tty;
}
