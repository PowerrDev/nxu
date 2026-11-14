/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/serial_test.c
 *
 * The serial terminal, /dev/console, tested through the real UART: QEMU's
 * serial port is the other end, and a script on the host types into it
 * (tools/serial_expect.pl running tools/serial_test.script).
 *
 * Each step prints "ready for <step>" once /dev/console is set up for it;
 * the script waits for that line before it types, because input that
 * arrives with no file open is discarded, as on any terminal. Then:
 *
 *   line     a line typed with a mistake and a rub-out, a carriage return
 *            for Enter: read back edited and ended by a newline (ICRNL);
 *            the host checks the echo it saw
 *   signal   ^C: the terminal asks for SIGINT (it has no foreground group
 *            to send it to, so it stays recorded)
 *   raw      VMIN 4 with no processing: four bytes, a NUL among them
 *   flood    6000 bytes typed while nobody reads, more than the receive
 *            ring and the terminal's input queue hold together: input is
 *            held back in the UART and the host waits, then every byte
 *            arrives, in order
 *
 * and in between, output written to /dev/console that the host must see,
 * with the line settings the UART cannot make refused.
 */

#include <kern/tests/serial_test.h>

#include <kern/console/console.h>
#include <kern/machine/timer.h>
#include <kern/process/signal.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/termios_defs.h>
#include <kern/tty/serial.h>
#include <kern/tty/tty.h>
#include <vfs/file.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERIAL_TEST_PATH "/dev/console"

/* As tools/serial_test.script types it. */
#define SERIAL_TEST_FLOOD 6000U

/* How long the flood is left unread, so it has to back up into the UART. */
#define SERIAL_TEST_STALL_US 1000000ULL

/* Longest a step waits for something the host does. */
#define SERIAL_TEST_TIMEOUT_US 10000000ULL

/* Static: a descriptor table is too large for a stack. */
static struct filedesc g_fd;
static uint8_t g_flood[SERIAL_TEST_FLOOD];

static bool
fail(const char *what)
{
	kprintf_for(serial_test_run, "FAILED: %s\n", what);
	return false;
}

static void
ready(const char *step)
{
	kprintf_for(serial_test_run, "ready for %s\n", step);
}

static struct nxu_termios
console_termios(uint32_t descriptor)
{
	struct nxu_termios termios;

	memset(&termios, 0, sizeof(termios));
	(void)vfs_ioctl(&g_fd, descriptor, NXU_TIOCGETA, &termios);
	return termios;
}

static bool
set_termios(uint32_t descriptor, struct nxu_termios *termios)
{
	return vfs_ioctl(&g_fd, descriptor, NXU_TIOCSETA, termios) == VFS_STATUS_OK;
}

/* Read until `size` bytes have arrived (a raw terminal returns what it has). */
static bool
read_all(uint32_t descriptor, uint8_t *buffer, uint32_t size)
{
	uint32_t done = 0U;

	while (done < size) {
		uint64_t count = 0ULL;

		if (vfs_read(&g_fd, descriptor, buffer + done, size - done, &count) != VFS_STATUS_OK || count == 0ULL)
			return false;

		done += (uint32_t)count;
	}

	return true;
}

static bool
test_line(uint32_t descriptor)
{
	uint8_t buffer[64];
	uint64_t count = 0ULL;

	ready("line");

	/* Typed as "hellp", rub-out, "o console", Enter. */
	if (vfs_read(&g_fd, descriptor, buffer, sizeof(buffer), &count) != VFS_STATUS_OK) return fail("read a line");
	if (count != 14ULL || memcmp(buffer, "hello console\n", 14U) != 0) return fail("the line read back");

	return true;
}

static bool
test_signal(tty_t tty)
{
	(void)tty_take_signals(tty);
	ready("signal");

	uint64_t deadline = timer_get_microseconds() + SERIAL_TEST_TIMEOUT_US;

	while (timer_get_microseconds() < deadline) {
		uint32_t signals = tty_take_signals(tty);

		if (signals == (1U << NXU_SIGINT)) return true;
		if (signals != 0U) return fail("^C asked for another signal");

		(void)sched_sleep_us(10000ULL);
	}

	return fail("^C asked for no signal");
}

static bool
test_raw(uint32_t descriptor)
{
	struct nxu_termios termios = console_termios(descriptor);
	uint8_t buffer[8];
	uint64_t count = 0ULL;

	termios.c_iflag = 0U;
	termios.c_lflag = 0U;
	termios.c_cc[NXU_VMIN] = 4U;
	termios.c_cc[NXU_VTIME] = 0U;
	if (!set_termios(descriptor, &termios)) return fail("raw mode");

	ready("raw");

	if (vfs_read(&g_fd, descriptor, buffer, sizeof(buffer), &count) != VFS_STATUS_OK) return fail("read raw");
	if (count != 4ULL || memcmp(buffer, "ab\0c", 4U) != 0) return fail("VMIN 4 read back");

	return true;
}

static bool
test_flood(uint32_t descriptor)
{
	struct nxu_termios termios = console_termios(descriptor);
	serial_stats_t before;
	serial_stats_t after;

	termios.c_cc[NXU_VMIN] = 1U;
	if (!set_termios(descriptor, &termios) || !serial_stats(&before)) return fail("flood setup");

	ready("flood");

	/* Nobody reads: the terminal's input queue and then the ring fill, and the rest waits in the UART. */
	(void)sched_sleep_us(SERIAL_TEST_STALL_US);

	if (!read_all(descriptor, g_flood, SERIAL_TEST_FLOOD)) return fail("read the flood");

	for (uint32_t index = 0U; index < SERIAL_TEST_FLOOD; index++) {
		if (g_flood[index] != (uint8_t)('a' + index % 26U)) {
			kprintf_for(serial_test_run, "byte %u is 0x%x\n", index, (unsigned)g_flood[index]);
			return fail("the flood arrived out of order or with bytes lost");
		}
	}

	if (!serial_stats(&after)) return fail("statistics");
	if (after.interrupt && after.held == before.held) return fail("the flood was never held back in the UART");
	if (after.overruns != before.overruns) return fail("the UART lost bytes");

	kprintf_for(serial_test_run, "%u bytes, input held back in the UART %llu time(s)\n", SERIAL_TEST_FLOOD,
				(unsigned long long)(after.held - before.held));
	return true;
}

static bool
test_output_and_line(uint32_t descriptor)
{
	static const char text[] = "serial_test_run: written through /dev/console\n";
	struct nxu_termios termios = console_termios(descriptor);
	uint64_t count = 0ULL;

	/* Cooked output: the host sees the newline as CR LF. */
	termios.c_oflag = NXU_OPOST | NXU_ONLCR;
	if (!set_termios(descriptor, &termios)) return fail("output modes");
	if (vfs_write(&g_fd, descriptor, text, sizeof(text) - 1U, &count) != VFS_STATUS_OK || count != sizeof(text) - 1U)
		return fail("write");

	struct nxu_termios odd = termios;

	odd.c_ispeed = 12345U;
	odd.c_ospeed = 12345U;
	if (set_termios(descriptor, &odd)) return fail("a speed the UART cannot make was taken");

	odd = termios;
	odd.c_ispeed = NXU_B9600;
	if (set_termios(descriptor, &odd)) return fail("split input and output speeds were taken");

	struct nxu_termios now = console_termios(descriptor);

	if (now.c_ospeed != NXU_B115200) return fail("the line does not report 115200");

	return true;
}

bool
serial_test_run(void)
{
	uint32_t descriptor;

	filedesc_init(&g_fd);

	tty_t tty = serial_tty();

	if (tty == TTY_NULL) return fail("no serial terminal");

	serial_stats_t stats;

	if (!serial_stats(&stats)) {
		tty_release(tty);
		return fail("statistics");
	}

	kprintf_for(serial_test_run, "line is %s\n", stats.interrupt ? "interrupt driven" : "polled");

	if (vfs_open(&g_fd, SERIAL_TEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE, &descriptor) != VFS_STATUS_OK) {
		tty_release(tty);
		return fail("open " SERIAL_TEST_PATH);
	}

	bool ok = test_line(descriptor) && test_signal(tty) && test_output_and_line(descriptor) && test_raw(descriptor) &&
			  test_flood(descriptor);

	/* Back to a cooked console for whoever opens it next. */
	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	termios.c_cflag |= NXU_CLOCAL;
	termios.c_ispeed = NXU_B115200;
	termios.c_ospeed = NXU_B115200;
	(void)set_termios(descriptor, &termios);

	if (vfs_close(&g_fd, descriptor) != VFS_STATUS_OK) ok = fail("close");

	tty_release(tty);
	return ok;
}
