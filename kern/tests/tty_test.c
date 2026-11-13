/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tty_test.c
 *
 * The terminal core, driven end to end: a loopback driver (everything
 * written comes back as input) behind a terminal registered in /dev, opened
 * and used through the same VFS entry points a system call reaches. Kernel
 * threads do the reads and writes that must sleep; the test itself (the
 * coordinator) only ever uses a descriptor opened "do not wait", so it can
 * never hang, and watches the threads with deadlines.
 *
 * What it checks:
 *
 *   - the node has the owner and mode it was registered with, and a second
 *     terminal cannot take the same name;
 *   - a failed first open leaves the terminal closed; the driver is opened
 *     on the first open and closed on the last, once each;
 *   - "would block", partial reads, readiness levels and edges, FIONREAD and
 *     TIOCOUTQ, NOT_TTY for an unknown request and for a non-terminal;
 *   - a blocked reader is woken by input;
 *   - flow control both ways: a full output queue makes a writer sleep, a
 *     full input queue holds the driver back until a read makes room, and a
 *     stream larger than both queues together arrives intact and in order;
 *   - the last close discards queued output;
 *   - hangup: a sleeping reader gets end of file, writes fail, new opens get
 *     NO_DEVICE, readiness says so; after the last close it opens again;
 *   - unregister and detach: the name and a vnode looked up before it can no
 *     longer open the terminal, files already open still work until the
 *     detach, and once the driver lets go the terminal is freed.
 *
 * With the line discipline (the loopback in raw mode for all of the above),
 * driven through a capture mode in which the driver keeps what it is given
 * instead of looping it back, and input is typed with tty_input:
 *
 *   - canonical lines, editing and its echo, end of file;
 *   - ^C: input and output flushed, ^C echoed, SIGINT asked for;
 *   - echo off (a password), output processing, VSTOP and VSTART;
 *   - VMIN and VTIME, each of the four combinations, timed by a sleeping
 *     reader.
 *
 * And the termios requests: TIOCGETA and TIOCSETA (the driver is asked and
 * can refuse), TIOCFLUSH, TIOCDRAIN and TIOCSETAW waiting for a stalled
 * driver, TIOCSETAF, TIOCSTOP and TIOCSTART, the window size and SIGWINCH,
 * CREAD.
 *
 * Threads are kernel threads of the kernel process, which run on the boot CPU
 * and are not preempted, so the order of events is the one written here.
 */

#include <kern/tests/tty_test.h>

#include <kern/tests/tty_loopback.h>

#include <kern/console/console.h>
#include <kern/ioready.h>
#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/syscall/termios_defs.h>
#include <kern/tty/tty.h>
#include <vfs/devfs.h>
#include <vfs/file.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TTY_TEST_NAME "ttyloop-test"
#define TTY_TEST_PATH "/dev/" TTY_TEST_NAME
#define TTY_TEST_TIMEOUT_US 5000000ULL
#define TTY_TEST_STREAM_SIZE (3U * TTY_OUTPUT_QUEUE_SIZE + 123U)

/* ---- the loopback driver (kern/tests/tty_loopback.h) -------------------- */

static tty_loopback_t g_loop;

static void
loop_set_stalled(bool stalled)
{
	tty_loopback_set_stalled(&g_loop, stalled);
}

/* ---- helpers ------------------------------------------------------------ */

/* Raw mode: bytes pass both ways unchanged, a read returns as soon as one is there. */
static void
set_raw(void)
{
	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	termios.c_iflag = 0U;
	termios.c_oflag = 0U;
	termios.c_lflag = 0U;
	termios.c_cc[NXU_VMIN] = 1U;
	termios.c_cc[NXU_VTIME] = 0U;
	(void)tty_set_termios(g_loop.tty, &termios);
}

/* Type `text` at the terminal. */
static void
type(const char *text)
{
	(void)tty_input(g_loop.tty, text, (uint32_t)strlen(text));
}

/* True when the driver has been given exactly `expected` since the last call. */
static bool
captured_is(const char *expected)
{
	return tty_loopback_captured_is(&g_loop, expected);
}

/* Static: a descriptor table is too large for a stack. */
static struct filedesc g_fd;

static bool
fail(const char *what)
{
	kprintf_for(tty_test_run, "FAILED: %s\n", what);
	return false;
}

static bool
wait_until(volatile bool *flag)
{
	uint64_t deadline = timer_get_microseconds() + TTY_TEST_TIMEOUT_US;

	while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
		if (timer_get_microseconds() > deadline) return false;

		(void)sched_yield();
	}

	return true;
}

/* Let everything runnable run for a while; true if `flag` stayed clear throughout. */
static bool
stays_clear(volatile bool *flag)
{
	uint64_t deadline = timer_get_microseconds() + 50000ULL;

	while (timer_get_microseconds() < deadline) {
		if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) return false;

		(void)sched_yield();
	}

	return !__atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

/* Every thread the test starts, released at the end once all have finished. */
#define TTY_TEST_MAX_THREADS 12U

static thread_t g_threads[TTY_TEST_MAX_THREADS];
static uint32_t g_thread_count;

static bool
start_thread(thread_continue_t function, void *argument)
{
	thread_t thread;

	if (g_thread_count >= TTY_TEST_MAX_THREADS) return false;

	if (!kernel_thread_create(proc_task(proc_kernel()), function, argument, &thread)) return false;

	g_threads[g_thread_count++] = thread;
	return sched_thread_start(thread);
}

static void
release_threads(void)
{
	for (uint32_t index = 0U; index < g_thread_count; index++)
		thread_deallocate(g_threads[index]);

	g_thread_count = 0U;
}

static uint32_t
ioctl_count(uint32_t descriptor, uint32_t command)
{
	uint32_t value = UINT32_MAX;

	if (vfs_ioctl(&g_fd, descriptor, command, &value) != VFS_STATUS_OK) return UINT32_MAX;

	return value;
}

static bool
level_is(uint32_t descriptor, uint32_t want)
{
	file_t file;
	uint32_t ready = 0U;
	struct ioready *source = 0;

	if (filedesc_get(&g_fd, descriptor, &file) != VFS_STATUS_OK) return false;

	bool ok = file_poll(file, &ready, &source) == VFS_STATUS_OK && source != 0 && ready == want;

	file_rele(file);
	return ok;
}

static bool
watch_attach(uint32_t descriptor, ioready_watch_t *watch, uint32_t events)
{
	file_t file;
	uint32_t ready = 0U;
	struct ioready *source = 0;

	if (filedesc_get(&g_fd, descriptor, &file) != VFS_STATUS_OK) return false;

	bool ok = file_poll(file, &ready, &source) == VFS_STATUS_OK && source != 0;

	if (ok) ioready_attach(source, watch, events, 0, 0);

	file_rele(file);
	return ok;
}

/* ---- threads ------------------------------------------------------------ */

/* A blocking read on one descriptor. */
static struct {
	uint32_t descriptor;
	char buffer[64];
	uint64_t count;
	vfs_status_t status;
	uint64_t elapsed_us;
	volatile bool started;
	volatile bool done;
} g_reader;

static void
reader_thread(void *argument)
{
	(void)argument;

	uint64_t begin = timer_get_microseconds();

	__atomic_store_n(&g_reader.started, true, __ATOMIC_RELEASE);
	g_reader.status = vfs_read(&g_fd, g_reader.descriptor, g_reader.buffer, sizeof(g_reader.buffer), &g_reader.count);
	g_reader.elapsed_us = timer_get_microseconds() - begin;
	__atomic_store_n(&g_reader.done, true, __ATOMIC_RELEASE);
}

/* Start a reader without requiring it to block (a read that may finish at once). */
static bool
reader_start(uint32_t descriptor)
{
	memset(&g_reader, 0, sizeof(g_reader));
	g_reader.descriptor = descriptor;

	return start_thread(reader_thread, 0) && wait_until(&g_reader.started);
}

static bool
reader_launch(uint32_t descriptor)
{
	memset(&g_reader, 0, sizeof(g_reader));
	g_reader.descriptor = descriptor;

	return start_thread(reader_thread, 0) && wait_until(&g_reader.started) && stays_clear(&g_reader.done);
}

/* One blocking write of a whole stream. */
static uint8_t g_stream[TTY_TEST_STREAM_SIZE];

static struct {
	uint32_t descriptor;
	uint64_t count;
	vfs_status_t status;
	volatile bool done;
} g_writer;

static void
writer_thread(void *argument)
{
	(void)argument;

	g_writer.status = vfs_write(&g_fd, g_writer.descriptor, g_stream, sizeof(g_stream), &g_writer.count);
	__atomic_store_n(&g_writer.done, true, __ATOMIC_RELEASE);
}

static uint8_t
stream_byte(uint32_t index)
{
	return (uint8_t)(index * 7U + index / 251U + 3U);
}

/* ---- the cases ---------------------------------------------------------- */

static bool
test_names(void)
{
	vnode_t vnode;
	vnode_attr_t attr;

	if (vfs_lookup(TTY_TEST_PATH, &vnode) != VFS_STATUS_OK) return fail("lookup");

	bool match = vnode_getattr(vnode, &attr) == VFS_STATUS_OK && attr.va_type == VNODE_TYPE_CHARACTER &&
				 attr.va_mode == (NXU_S_IFCHR | 0620U) && attr.va_uid == 0U && attr.va_gid == 4U;

	vnode_rele(vnode);
	if (!match) return fail("registered attributes");

	uint32_t before = tty_count();
	tty_t other;

	if (!tty_create(&tty_loopback_operations, 0, &other)) return fail("second terminal");
	if (tty_register(other, TTY_TEST_NAME, 0U, 0U, 0666U)) return fail("duplicate name accepted");

	tty_release(other);
	if (tty_count() != before) return fail("unregistered terminal not freed");

	return true;
}

/* Open a closed terminal twice (b "do not wait"), after the driver has refused once. */
static bool
test_open_close(uint32_t *a, uint32_t *b)
{
	uint32_t descriptor;
	uint32_t opens = g_loop.opens;
	uint32_t closes = g_loop.closes;

	g_loop.fail_open = true;
	if (vfs_open(&g_fd, TTY_TEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE, &descriptor) != VFS_STATUS_BUSY)
		return fail("a driver's refusal did not fail the open");

	g_loop.fail_open = false;
	if (g_loop.opens != opens || g_loop.closes != closes) return fail("refused open counted");

	if (vfs_open(&g_fd, TTY_TEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE, a) != VFS_STATUS_OK) return fail("open");
	if (vfs_open(&g_fd, TTY_TEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_NONBLOCK, b) != VFS_STATUS_OK)
		return fail("second open");
	if (g_loop.opens != opens + 1U) return fail("driver not opened exactly once");

	return true;
}

static bool
test_basic_io(uint32_t a, uint32_t b)
{
	char buffer[64];
	uint64_t count = 0ULL;

	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_WOULD_BLOCK)
		return fail("empty nonblocking read");
	if (ioctl_count(b, NXU_FIONREAD) != 0U) return fail("FIONREAD empty");
	if (!level_is(b, IOREADY_WRITE)) return fail("level of an idle terminal");

	ioready_watch_t watch = {0};

	if (!watch_attach(b, &watch, IOREADY_READ)) return fail("attach");

	if (vfs_write(&g_fd, a, "hello", 5ULL, &count) != VFS_STATUS_OK || count != 5ULL) return fail("write");

	bool fired = (ioready_watch_fired(&watch) & IOREADY_READ) != 0U;

	ioready_detach(&watch);
	if (!fired) return fail("input did not post readable");

	if (ioctl_count(a, NXU_FIONREAD) != 5U) return fail("FIONREAD after write");
	if (ioctl_count(a, NXU_TIOCOUTQ) != 0U) return fail("TIOCOUTQ after the driver took everything");
	if (!level_is(b, IOREADY_READ | IOREADY_WRITE)) return fail("level with input queued");

	if (vfs_read(&g_fd, b, buffer, 2ULL, &count) != VFS_STATUS_OK || count != 2ULL || memcmp(buffer, "he", 2U) != 0)
		return fail("partial read");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 3ULL ||
		memcmp(buffer, "llo", 3U) != 0)
		return fail("rest of the input");

	uint32_t unknown = 0U;

	if (vfs_ioctl(&g_fd, a, NXU_IOR('t', 250U, uint32_t), &unknown) != VFS_STATUS_NOT_TTY)
		return fail("unknown request");

	uint32_t directory;

	if (vfs_open(&g_fd, "/dev", VFS_OPEN_READ, &directory) != VFS_STATUS_OK) return fail("open /dev");

	vfs_status_t status = vfs_ioctl(&g_fd, directory, NXU_FIONREAD, &unknown);

	(void)vfs_close(&g_fd, directory);
	if (status != VFS_STATUS_NOT_TTY) return fail("a directory answered a terminal request");

	return true;
}

static bool
test_blocked_reader(uint32_t a, uint32_t b)
{
	uint64_t count = 0ULL;

	if (!reader_launch(a)) return fail("reader did not block on an empty terminal");
	if (vfs_write(&g_fd, b, "ping", 4ULL, &count) != VFS_STATUS_OK || count != 4ULL) return fail("write to reader");
	if (!wait_until(&g_reader.done)) return fail("input did not wake the reader");
	if (g_reader.status != VFS_STATUS_OK || g_reader.count != 4ULL || memcmp(g_reader.buffer, "ping", 4U) != 0)
		return fail("reader got the wrong bytes");

	return true;
}

static bool
test_flow(uint32_t a, uint32_t b)
{
	uint64_t count = 0ULL;

	/* The driver takes nothing: the output queue fills and stops a nonblocking writer. */
	loop_set_stalled(true);

	memset(g_stream, 'x', sizeof(g_stream));
	if (vfs_write(&g_fd, b, g_stream, TTY_OUTPUT_QUEUE_SIZE + 100ULL, &count) != VFS_STATUS_OK ||
		count != TTY_OUTPUT_QUEUE_SIZE)
		return fail("short write to a filling output queue");
	if (vfs_write(&g_fd, b, g_stream, 1ULL, &count) != VFS_STATUS_WOULD_BLOCK) return fail("write to a full queue");
	if (ioctl_count(b, NXU_TIOCOUTQ) != TTY_OUTPUT_QUEUE_SIZE) return fail("TIOCOUTQ when full");
	if (!level_is(b, 0U)) return fail("level of a full, unread terminal");

	/* Drain it through the input side. */
	loop_set_stalled(false);

	uint32_t drained = 0U;
	uint8_t buffer[512];

	while (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) == VFS_STATUS_OK) {
		drained += (uint32_t)count;
	}

	if (drained != TTY_OUTPUT_QUEUE_SIZE) return fail("drained count");

	/* A stream bigger than both queues, written by a thread that must sleep, read here. */
	for (uint32_t index = 0U; index < sizeof(g_stream); index++)
		g_stream[index] = stream_byte(index);

	memset(&g_writer, 0, sizeof(g_writer));
	g_writer.descriptor = a;

	uint32_t unthrottles = g_loop.unthrottles;

	if (!start_thread(writer_thread, 0)) return fail("writer thread");

	uint32_t received = 0U;
	uint64_t deadline = timer_get_microseconds() + TTY_TEST_TIMEOUT_US;

	while (received < sizeof(g_stream)) {
		vfs_status_t status = vfs_read(&g_fd, b, buffer, sizeof(buffer), &count);

		if (status == VFS_STATUS_WOULD_BLOCK) {
			if (timer_get_microseconds() > deadline) return fail("stream stalled");

			(void)sched_yield();
			continue;
		}

		if (status != VFS_STATUS_OK || count == 0ULL) return fail("stream read");

		for (uint32_t index = 0U; index < count; index++) {
			if (received + index >= sizeof(g_stream) || buffer[index] != stream_byte(received + index))
				return fail("stream corrupted or out of order");
		}

		received += (uint32_t)count;
	}

	if (!wait_until(&g_writer.done)) return fail("writer never finished");
	if (g_writer.status != VFS_STATUS_OK || g_writer.count != sizeof(g_stream)) return fail("writer's count");
	if (g_loop.unthrottles == unthrottles) return fail("a full input queue was never unthrottled");

	return true;
}

static void
set_lflag(nxu_tcflag_t clear, nxu_tcflag_t add)
{
	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	termios.c_lflag = (termios.c_lflag & ~clear) | add;
	(void)tty_set_termios(g_loop.tty, &termios);
}

/* The default discipline, as a person at a keyboard meets it. */
static bool
test_discipline(uint32_t b)
{
	uint8_t buffer[64];
	uint64_t count = 0ULL;

	g_loop.capture = true;
	g_loop.captured_length = 0U;
	set_lflag(0U, 0U);

	/* A line, edited. */
	type("ab\x7f" "c");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_WOULD_BLOCK)
		return fail("a canonical read returned before the line ended");
	if (!level_is(b, IOREADY_WRITE)) return fail("an unfinished line reads as readable");

	type("\r");
	if (!level_is(b, IOREADY_READ | IOREADY_WRITE)) return fail("a finished line is not readable");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 3ULL ||
		memcmp(buffer, "ac\n", 3U) != 0)
		return fail("canonical line");
	if (!captured_is("ab\b \bc\r\n")) return fail("echo of an edited line");

	/* End of file. */
	type("\x04");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 0ULL)
		return fail("VEOF is not end of file");

	/* ^C: what was typed and what was waiting to be shown are gone. */
	loop_set_stalled(true);
	if (vfs_write(&g_fd, b, "unseen", 6ULL, &count) != VFS_STATUS_OK) return fail("write before ^C");

	type("abc\x03");

	if (ioctl_count(b, NXU_FIONREAD) != 0U) return fail("^C left input queued");
	if (ioctl_count(b, NXU_TIOCOUTQ) != 2U) return fail("^C did not flush output (or echo ^C)");
	if (tty_take_signals(g_loop.tty) != (1U << NXU_SIGINT)) return fail("^C did not ask for SIGINT");
	if (tty_take_signals(g_loop.tty) != 0U) return fail("signals not forgotten once taken");

	loop_set_stalled(false);
	if (!captured_is("^C")) return fail("echo of ^C");

	/* A password: nothing echoes, the line still arrives. */
	set_lflag(NXU_ECHO, 0U);
	type("hunter2\r");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 8ULL ||
		memcmp(buffer, "hunter2\n", 8U) != 0)
		return fail("line typed with echo off");
	if (!captured_is("")) return fail("echo off still echoed");
	set_lflag(0U, 0U);

	/* Output processing. */
	if (vfs_write(&g_fd, b, "a\nb", 3ULL, &count) != VFS_STATUS_OK || count != 3ULL) return fail("write");
	if (!captured_is("a\r\nb")) return fail("ONLCR");

	/* ^S holds output back, ^Q lets it go. */
	type("\x13");
	if (vfs_write(&g_fd, b, "zz", 2ULL, &count) != VFS_STATUS_OK) return fail("write while stopped");
	if (!captured_is("") || ioctl_count(b, NXU_TIOCOUTQ) != 2U) return fail("output went out while stopped");
	type("\x11");
	if (!captured_is("zz")) return fail("output not restarted by VSTART");

	g_loop.capture = false;
	set_raw();
	return true;
}

static void
set_min_time(uint8_t min, uint8_t time)
{
	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	termios.c_iflag = 0U;
	termios.c_oflag = 0U;
	termios.c_lflag = 0U;
	termios.c_cc[NXU_VMIN] = min;
	termios.c_cc[NXU_VTIME] = time;
	(void)tty_set_termios(g_loop.tty, &termios);
}

/* VMIN and VTIME, with a reader that really sleeps; times are checked loosely (a tick or two). */
static bool
test_min_time(uint32_t a)
{
	uint64_t count = 1ULL;
	uint8_t buffer[8];

	g_loop.capture = true;

	/* 0, 0: return at once with what is there, even nothing. */
	set_min_time(0U, 0U);
	if (vfs_read(&g_fd, a, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 0ULL)
		return fail("VMIN 0 VTIME 0 waited or failed");

	/* 0, 2: wait up to 0.2 s for anything. */
	set_min_time(0U, 2U);
	if (!reader_start(a) || !wait_until(&g_reader.done)) return fail("VMIN 0 VTIME 2 never returned");
	if (g_reader.status != VFS_STATUS_OK || g_reader.count != 0ULL) return fail("VMIN 0 VTIME 2 timeout result");
	if (g_reader.elapsed_us < 190000ULL || g_reader.elapsed_us > 2000000ULL)
		return fail("VMIN 0 VTIME 2 did not wait about 0.2 s");

	if (!reader_launch(a)) return fail("VMIN 0 VTIME 2 did not wait");
	type("q");
	if (!wait_until(&g_reader.done) || g_reader.count != 1ULL || g_reader.elapsed_us > 190000ULL)
		return fail("VMIN 0 VTIME 2 did not return at the first byte");

	/* 3, 0: wait for three bytes, however long. */
	set_min_time(3U, 0U);
	if (!reader_launch(a)) return fail("VMIN 3 did not wait");
	type("xy");
	if (!stays_clear(&g_reader.done)) return fail("VMIN 3 returned with two bytes");
	type("z");
	if (!wait_until(&g_reader.done) || g_reader.status != VFS_STATUS_OK || g_reader.count != 3ULL ||
		memcmp(g_reader.buffer, "xyz", 3U) != 0)
		return fail("VMIN 3 result");

	/* 4, 1: an inter-byte timer of 0.1 s that starts at the first byte. */
	set_min_time(4U, 1U);
	if (!reader_launch(a)) return fail("VMIN 4 VTIME 1 did not wait for the first byte");
	type("k");
	if (!wait_until(&g_reader.done) || g_reader.status != VFS_STATUS_OK || g_reader.count != 1ULL)
		return fail("VMIN 4 VTIME 1 did not time out after a byte");

	/* Raw mode echoes nothing: the driver must have been given nothing. */
	bool quiet = captured_is("");

	g_loop.capture = false;
	set_raw();
	return quiet || fail("raw mode produced output");
}

/* One ioctl made by a thread, for the requests that sleep. */
static struct {
	uint32_t descriptor;
	uint32_t command;
	struct nxu_termios termios;
	vfs_status_t status;
	volatile bool started;
	volatile bool done;
} g_ioctl;

static void
ioctl_thread(void *argument)
{
	(void)argument;

	__atomic_store_n(&g_ioctl.started, true, __ATOMIC_RELEASE);
	g_ioctl.status = vfs_ioctl(&g_fd, g_ioctl.descriptor, g_ioctl.command, &g_ioctl.termios);
	__atomic_store_n(&g_ioctl.done, true, __ATOMIC_RELEASE);
}

/* Start `command` on `descriptor` in a thread and require it to block. */
static bool
ioctl_blocks(uint32_t descriptor, uint32_t command, const struct nxu_termios *termios)
{
	memset(&g_ioctl, 0, sizeof(g_ioctl));
	g_ioctl.descriptor = descriptor;
	g_ioctl.command = command;
	if (termios != 0) g_ioctl.termios = *termios;

	return start_thread(ioctl_thread, 0) && wait_until(&g_ioctl.started) && stays_clear(&g_ioctl.done);
}

static struct nxu_termios
current_termios(uint32_t descriptor)
{
	struct nxu_termios termios;

	memset(&termios, 0xA5, sizeof(termios));
	(void)vfs_ioctl(&g_fd, descriptor, NXU_TIOCGETA, &termios);
	return termios;
}

static bool
termios_equal(const struct nxu_termios *left, const struct nxu_termios *right)
{
	return memcmp(left, right, sizeof(*left)) == 0;
}

/* The termios requests, through the same entry point the system call uses. */
static bool
test_requests(uint32_t a, uint32_t b)
{
	uint64_t count = 0ULL;
	uint8_t buffer[16];

	/* Read and set; the driver is asked; a refusal changes nothing. */
	struct nxu_termios before = current_termios(a);
	struct nxu_termios wanted = before;

	wanted.c_cc[NXU_VMIN] = 5U;
	wanted.c_ospeed = NXU_B38400;

	uint32_t params = g_loop.params;

	if (vfs_ioctl(&g_fd, a, NXU_TIOCSETA, &wanted) != VFS_STATUS_OK) return fail("TIOCSETA");

	struct nxu_termios now = current_termios(b);

	if (!termios_equal(&now, &wanted)) return fail("TIOCGETA after TIOCSETA, on another descriptor");
	if (g_loop.params != params + 1U) return fail("the driver was not asked about the change");

	struct nxu_termios refused = wanted;

	refused.c_ospeed = 1234U;
	g_loop.refuse_speed = 1234U;

	if (vfs_ioctl(&g_fd, a, NXU_TIOCSETA, &refused) != VFS_STATUS_INVALID) return fail("a refused setting was taken");

	now = current_termios(a);
	g_loop.refuse_speed = 0U;
	if (!termios_equal(&now, &wanted)) return fail("a refused setting changed the terminal");

	set_raw();

	/* Flush: input, output, both, and nothing else. */
	if (vfs_write(&g_fd, b, "abc", 3ULL, &count) != VFS_STATUS_OK || ioctl_count(b, NXU_FIONREAD) != 3U)
		return fail("loop before TIOCFLUSH");

	uint32_t which = NXU_FREAD;

	if (vfs_ioctl(&g_fd, b, NXU_TIOCFLUSH, &which) != VFS_STATUS_OK || ioctl_count(b, NXU_FIONREAD) != 0U)
		return fail("TIOCFLUSH FREAD");

	loop_set_stalled(true);
	if (vfs_write(&g_fd, b, "xyz", 3ULL, &count) != VFS_STATUS_OK || ioctl_count(b, NXU_TIOCOUTQ) != 3U)
		return fail("write to a stalled driver");

	which = NXU_FWRITE;
	if (vfs_ioctl(&g_fd, b, NXU_TIOCFLUSH, &which) != VFS_STATUS_OK || ioctl_count(b, NXU_TIOCOUTQ) != 0U)
		return fail("TIOCFLUSH FWRITE");

	which = 4U;
	if (vfs_ioctl(&g_fd, b, NXU_TIOCFLUSH, &which) != VFS_STATUS_INVALID) return fail("TIOCFLUSH took a bad queue");

	/* Drain waits for the driver; so does TIOCSETAW, which then applies. */
	if (vfs_write(&g_fd, b, "dr", 2ULL, &count) != VFS_STATUS_OK) return fail("write before drain");
	if (!ioctl_blocks(a, NXU_TIOCDRAIN, 0)) return fail("TIOCDRAIN did not wait for the driver");

	loop_set_stalled(false);
	if (!wait_until(&g_ioctl.done) || g_ioctl.status != VFS_STATUS_OK) return fail("TIOCDRAIN never finished");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 2ULL)
		return fail("drained bytes did not arrive");

	if (vfs_ioctl(&g_fd, a, NXU_TIOCDRAIN, 0) != VFS_STATUS_OK) return fail("TIOCDRAIN with nothing queued");

	wanted = current_termios(a);
	wanted.c_cc[NXU_VTIME] = 3U;

	loop_set_stalled(true);
	if (vfs_write(&g_fd, b, "w", 1ULL, &count) != VFS_STATUS_OK) return fail("write before TIOCSETAW");
	if (!ioctl_blocks(a, NXU_TIOCSETAW, &wanted)) return fail("TIOCSETAW did not wait for output");

	now = current_termios(b);
	if (now.c_cc[NXU_VTIME] != 0U) return fail("TIOCSETAW applied before output drained");

	loop_set_stalled(false);
	if (!wait_until(&g_ioctl.done) || g_ioctl.status != VFS_STATUS_OK) return fail("TIOCSETAW never finished");

	now = current_termios(b);
	if (now.c_cc[NXU_VTIME] != 3U) return fail("TIOCSETAW did not apply");

	/* TIOCSETAF: unread input goes; the "w" that came back is exactly that. */
	set_raw();
	if (ioctl_count(b, NXU_FIONREAD) != 1U) return fail("input before TIOCSETAF");

	wanted = current_termios(a);
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSETAF, &wanted) != VFS_STATUS_OK || ioctl_count(b, NXU_FIONREAD) != 0U)
		return fail("TIOCSETAF kept unread input");

	/* Stop and start output from a program. */
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSTOP, 0) != VFS_STATUS_OK) return fail("TIOCSTOP");
	if (vfs_write(&g_fd, b, "s", 1ULL, &count) != VFS_STATUS_OK || ioctl_count(b, NXU_TIOCOUTQ) != 1U)
		return fail("output went out while stopped by TIOCSTOP");
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSTART, 0) != VFS_STATUS_OK || ioctl_count(b, NXU_TIOCOUTQ) != 0U ||
		ioctl_count(b, NXU_FIONREAD) != 1U)
		return fail("TIOCSTART did not restart output");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK) return fail("read after TIOCSTART");

	/* Window size, and SIGWINCH only for a real change. */
	struct nxu_winsize size = {.ws_row = 0xFFFFU};

	(void)tty_take_signals(g_loop.tty);
	if (vfs_ioctl(&g_fd, a, NXU_TIOCGWINSZ, &size) != VFS_STATUS_OK || size.ws_row != 0U || size.ws_col != 0U)
		return fail("a new terminal has a window size");

	size = (struct nxu_winsize){.ws_row = 24U, .ws_col = 80U, .ws_xpixel = 640U, .ws_ypixel = 480U};
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSWINSZ, &size) != VFS_STATUS_OK) return fail("TIOCSWINSZ");
	if (tty_take_signals(g_loop.tty) != (1U << NXU_SIGWINCH)) return fail("a new window size did not ask for SIGWINCH");
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSWINSZ, &size) != VFS_STATUS_OK || tty_take_signals(g_loop.tty) != 0U)
		return fail("the same window size asked for SIGWINCH");

	struct nxu_winsize back;

	if (vfs_ioctl(&g_fd, b, NXU_TIOCGWINSZ, &back) != VFS_STATUS_OK || memcmp(&back, &size, sizeof(size)) != 0)
		return fail("TIOCGWINSZ");

	/* With the receiver off, input is dropped. */
	wanted = current_termios(a);
	wanted.c_cflag &= ~NXU_CREAD;
	if (vfs_ioctl(&g_fd, a, NXU_TIOCSETA, &wanted) != VFS_STATUS_OK) return fail("CREAD off");
	if (vfs_write(&g_fd, b, "q", 1ULL, &count) != VFS_STATUS_OK || ioctl_count(b, NXU_FIONREAD) != 0U)
		return fail("input arrived with CREAD off");

	set_raw();
	return true;
}

static bool
test_last_close(uint32_t *a, uint32_t *b)
{
	uint64_t count = 0ULL;

	loop_set_stalled(true);
	if (vfs_write(&g_fd, *b, "stale", 5ULL, &count) != VFS_STATUS_OK) return fail("write before close");

	if (vfs_close(&g_fd, *a) != VFS_STATUS_OK) return fail("close");
	if (g_loop.closes != 0U) return fail("driver closed with a file still open");
	if (vfs_close(&g_fd, *b) != VFS_STATUS_OK) return fail("last close");
	if (g_loop.closes != 1U) return fail("driver not closed on the last close");

	loop_set_stalled(false);

	if (!test_open_close(a, b)) return false;
	if (g_loop.opens != 2U) return fail("reopen did not reopen the driver");
	if (ioctl_count(*a, NXU_TIOCOUTQ) != 0U || ioctl_count(*a, NXU_FIONREAD) != 0U)
		return fail("last close left bytes queued");

	return true;
}

static bool
test_hangup(uint32_t *a, uint32_t *b)
{
	uint64_t count = 0ULL;
	ioready_watch_t watch = {0};
	uint32_t descriptor;

	if (!watch_attach(*b, &watch, 0U)) return fail("attach for hangup");
	if (!reader_launch(*a)) return fail("reader did not block before the hangup");

	tty_hangup(g_loop.tty);

	if (!wait_until(&g_reader.done)) return fail("hangup did not wake the reader");
	if (g_reader.status != VFS_STATUS_OK || g_reader.count != 0ULL) return fail("reader did not see end of file");

	bool fired = (ioready_watch_fired(&watch) & IOREADY_HANGUP) != 0U;

	ioready_detach(&watch);
	if (!fired) return fail("hangup not posted");

	if (vfs_write(&g_fd, *b, "x", 1ULL, &count) != VFS_STATUS_IO_ERROR) return fail("write after hangup");
	if (!level_is(*b, IOREADY_READ | IOREADY_HANGUP)) return fail("level after hangup");
	if (vfs_open(&g_fd, TTY_TEST_PATH, VFS_OPEN_READ, &descriptor) != VFS_STATUS_NO_DEVICE)
		return fail("open of a hung-up terminal");

	/* Once the session's files are gone the line is fresh again. */
	if (vfs_close(&g_fd, *a) != VFS_STATUS_OK || vfs_close(&g_fd, *b) != VFS_STATUS_OK)
		return fail("close after hangup");
	if (!test_open_close(a, b)) return fail("reopen after hangup");
	if (vfs_write(&g_fd, *a, "up", 2ULL, &count) != VFS_STATUS_OK || count != 2ULL)
		return fail("write after reopening");

	char buffer[8];

	if (vfs_read(&g_fd, *b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 2ULL)
		return fail("read after reopening");

	return true;
}

static bool
test_unregister_detach(uint32_t a, uint32_t b, uint32_t baseline)
{
	uint64_t count = 0ULL;
	vnode_t held;
	vnode_t gone;
	vnode_t opened;
	char buffer[8];

	if (vfs_lookup(TTY_TEST_PATH, &held) != VFS_STATUS_OK) return fail("lookup before unregister");

	tty_unregister(g_loop.tty);

	bool refused = vfs_lookup(TTY_TEST_PATH, &gone) == VFS_STATUS_NOT_FOUND &&
				   vnode_open(held, VFS_OPEN_READ, &opened) == VFS_STATUS_NOT_FOUND;

	vnode_rele(held);
	if (!refused) return fail("unregistered terminal could still be opened");

	if (vfs_write(&g_fd, a, "z", 1ULL, &count) != VFS_STATUS_OK ||
		vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 1ULL || buffer[0] != 'z')
		return fail("open files stopped working at unregister");

	/* The driver goes away: it lets go of the terminal, which lives on in the files. */
	uint32_t closes = g_loop.closes;

	tty_detach(g_loop.tty);
	tty_release(g_loop.tty);

	if (g_loop.released) return fail("terminal freed with files open");
	if (vfs_read(&g_fd, b, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 0ULL)
		return fail("read after detach is not end of file");

	if (vfs_close(&g_fd, a) != VFS_STATUS_OK || vfs_close(&g_fd, b) != VFS_STATUS_OK) return fail("final close");
	if (g_loop.closes != closes) return fail("detached driver was closed");
	if (!g_loop.released || tty_count() != baseline) return fail("terminal not freed after its last file");

	return true;
}

bool
tty_test_run(void)
{
	uint32_t baseline = tty_count();
	uint32_t devices = devfs_device_count();
	uint32_t a;
	uint32_t b;

	filedesc_init(&g_fd);
	g_thread_count = 0U;

	if (!tty_loopback_create(&g_loop)) return fail("create");
	if (!tty_register(g_loop.tty, TTY_TEST_NAME, 0U, 4U, 0620U)) return fail("register");

	/* The cases before the discipline's own need bytes to pass unchanged. */
	set_raw();

	bool ok = test_names() && test_open_close(&a, &b) && test_basic_io(a, b) && test_blocked_reader(a, b) &&
			  test_flow(a, b) && test_discipline(b) && test_min_time(a) && test_requests(a, b) &&
			  test_last_close(&a, &b) &&
			  test_hangup(&a, &b) &&
			  test_unregister_detach(a, b, baseline);

	/* Only after a pass is every thread known to have finished. */
	if (!ok) return false;

	release_threads();
	if (devfs_device_count() != devices) return fail("device count after the test");

	return true;
}
