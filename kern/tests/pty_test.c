/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/pty_test.c
 *
 * Pseudo-terminal pairs, driven end to end: /dev/ptmx opened through the
 * VFS as a terminal emulator would, the slave opened by the name the master
 * gives, and both used through the entry points a system call reaches. Kernel
 * threads do the reads and writes that must sleep; the test itself (the
 * coordinator) only uses descriptors opened "do not wait", so it can never
 * hang, and watches the threads with deadlines.
 *
 * What it checks:
 *
 *   - every open of /dev/ptmx makes a new pair with its own slave name; the
 *     slave belongs to the opener, group tty, mode 0620;
 *   - the slave refuses to open until TIOCPTYUNLK;
 *   - typing at the master reaches the slave as a canonical line, with its
 *     echo coming back to the master; the slave's output reaches the master
 *     through output processing; FIONREAD on the master counts it;
 *   - echo turned off from the master (a password prompt) echoes nothing;
 *   - the window size set on the master is the slave's;
 *   - a master blocked in a read is woken by the slave's output;
 *   - a stream larger than the master's ring and the terminal's queue
 *     together arrives whole and in order, the slave's writer waiting for
 *     the master to read;
 *   - the slave's last close: what it wrote just before still reaches the
 *     master, then end of file, and readiness reports a hangup;
 *   - the master's close: the slave hangs up for good (end of file, writes
 *     fail, its name is gone), and the pair is freed with its last file;
 *   - a pair whose slave was never opened is freed by the master's close.
 *
 * Threads are kernel threads of the kernel process, which run on the boot CPU
 * and are not preempted, so the order of events is the one written here.
 */

#include <kern/tests/pty_test.h>

#include <kern/console/console.h>
#include <kern/ioready.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/syscall/termios_defs.h>
#include <kern/tty/pty.h>
#include <kern/tty/tty.h>
#include <vfs/devfs.h>
#include <vfs/file.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PTY_TEST_TIMEOUT_US 5000000ULL

/* Larger than the master's ring and the terminal's output queue together. */
#define PTY_TEST_STREAM_SIZE (3U * TTY_OUTPUT_QUEUE_SIZE + 77U)

#define PTY_TEST_OPEN (VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_NOCTTY)

/* Static: a descriptor table is too large for a stack. */
static struct filedesc g_fd;

static bool
fail(const char *what)
{
	kprintf_for(pty_test_run, "FAILED: %s\n", what);
	return false;
}

static bool
wait_until(volatile bool *flag)
{
	uint64_t deadline = timer_get_microseconds() + PTY_TEST_TIMEOUT_US;

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
#define PTY_TEST_MAX_THREADS 4U

static thread_t g_threads[PTY_TEST_MAX_THREADS];
static uint32_t g_thread_count;

static bool
start_thread(thread_continue_t function, void *argument)
{
	thread_t thread;

	if (g_thread_count >= PTY_TEST_MAX_THREADS) return false;

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

/* Read exactly `expected` from a "do not wait" descriptor, in one read. */
static bool
reads(uint32_t descriptor, const char *expected)
{
	char buffer[64];
	uint64_t count = 0ULL;
	size_t length = strlen(expected);

	if (vfs_read(&g_fd, descriptor, buffer, sizeof(buffer), &count) != VFS_STATUS_OK) return false;

	return count == length && memcmp(buffer, expected, length) == 0;
}

static bool
writes(uint32_t descriptor, const char *text)
{
	uint64_t count = 0ULL;
	size_t length = strlen(text);

	return vfs_write(&g_fd, descriptor, text, length, &count) == VFS_STATUS_OK && count == length;
}

static bool
nothing_to_read(uint32_t descriptor)
{
	char buffer[8];
	uint64_t count = 0ULL;

	return vfs_read(&g_fd, descriptor, buffer, sizeof(buffer), &count) == VFS_STATUS_WOULD_BLOCK;
}

/* Change the slave's local modes through the master, as an emulator or a test of one does. */
static bool
set_modes(uint32_t master, uint32_t lflag, uint32_t oflag)
{
	struct nxu_termios termios;

	if (vfs_ioctl(&g_fd, master, NXU_TIOCGETA, &termios) != VFS_STATUS_OK) return false;

	termios.c_lflag = lflag;
	termios.c_oflag = oflag;
	termios.c_cc[NXU_VMIN] = 1U;
	termios.c_cc[NXU_VTIME] = 0U;
	return vfs_ioctl(&g_fd, master, NXU_TIOCSETA, &termios) == VFS_STATUS_OK;
}

/* Open /dev/ptmx "do not wait" and learn the slave's path. */
static bool
open_pair(uint32_t *master, nxu_ptyname_t *name)
{
	if (vfs_open(&g_fd, "/dev/ptmx", PTY_TEST_OPEN | VFS_OPEN_NONBLOCK, master) != VFS_STATUS_OK) return false;

	return vfs_ioctl(&g_fd, *master, NXU_TIOCPTYGNAME, name) == VFS_STATUS_OK;
}

/* ---- threads ------------------------------------------------------------ */

/* One blocking read of the master. */
static struct {
	uint32_t descriptor;
	char buffer[64];
	uint64_t count;
	vfs_status_t status;
	volatile bool started;
	volatile bool done;
} g_reader;

static void
reader_thread(void *argument)
{
	(void)argument;

	__atomic_store_n(&g_reader.started, true, __ATOMIC_RELEASE);
	g_reader.status = vfs_read(&g_fd, g_reader.descriptor, g_reader.buffer, sizeof(g_reader.buffer), &g_reader.count);
	__atomic_store_n(&g_reader.done, true, __ATOMIC_RELEASE);
}

/* One blocking write of a whole stream to the slave. */
static uint8_t g_stream[PTY_TEST_STREAM_SIZE];

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
	return (uint8_t)(index * 13U + index / 241U + 5U);
}

/* ---- the cases ---------------------------------------------------------- */

/* Two pairs, two names; the slave's attributes; locked until unlocked. Leaves the first pair's slave open. */
static bool
test_make(uint32_t *master, uint32_t *slave, uint32_t *blocking_slave, nxu_ptyname_t *name, uint32_t baseline)
{
	uint32_t other;
	nxu_ptyname_t other_name;

	if (!open_pair(master, name)) return fail("open /dev/ptmx");
	if (!open_pair(&other, &other_name)) return fail("open /dev/ptmx a second time");
	if (pty_count() != baseline + 2U) return fail("pair count after two opens");
	if (memcmp(name->name, "/dev/ttys", 9U) != 0 || strcmp(name->name, other_name.name) == 0)
		return fail("slave names");

	vnode_t vnode;
	vnode_attr_t attr;

	if (vfs_lookup(name->name, &vnode) != VFS_STATUS_OK) return fail("lookup of the slave");

	bool match = vnode_getattr(vnode, &attr) == VFS_STATUS_OK && attr.va_type == VNODE_TYPE_CHARACTER &&
				 attr.va_mode == (NXU_S_IFCHR | 0620U) && attr.va_uid == 0U && attr.va_gid == PTY_TTY_GID;

	vnode_rele(vnode);
	if (!match) return fail("slave attributes");

	/* The second pair is never used: its master's close alone frees it. */
	if (vfs_close(&g_fd, other) != VFS_STATUS_OK) return fail("close of an unused master");
	if (pty_count() != baseline + 1U) return fail("an unused pair was not freed");
	if (vfs_lookup(other_name.name, &vnode) != VFS_STATUS_NOT_FOUND) return fail("an unused pair kept its name");

	if (vfs_open(&g_fd, name->name, PTY_TEST_OPEN | VFS_OPEN_NONBLOCK, slave) != VFS_STATUS_DENIED)
		return fail("the slave opened while locked");

	if (vfs_ioctl(&g_fd, *master, NXU_TIOCPTYGRANT, 0) != VFS_STATUS_OK) return fail("TIOCPTYGRANT");
	if (vfs_ioctl(&g_fd, *master, NXU_TIOCPTYUNLK, 0) != VFS_STATUS_OK) return fail("TIOCPTYUNLK");
	if (vfs_open(&g_fd, name->name, PTY_TEST_OPEN | VFS_OPEN_NONBLOCK, slave) != VFS_STATUS_OK)
		return fail("open of the unlocked slave");
	if (vfs_open(&g_fd, name->name, PTY_TEST_OPEN, blocking_slave) != VFS_STATUS_OK)
		return fail("second open of the slave");

	return true;
}

/* Typing, echo, output processing, a password, the window size. */
static bool
test_io(uint32_t master, uint32_t slave)
{
	if (!nothing_to_read(master)) return fail("the master read something before anyone wrote");
	if (!level_is(master, IOREADY_WRITE)) return fail("an idle master's readiness");

	/* A line typed at the master: the slave gets it whole, the master sees it echoed. */
	if (!writes(master, "hi")) return fail("typing");
	if (!nothing_to_read(slave)) return fail("the slave read an unfinished line");
	if (!writes(master, "\r")) return fail("typing return");
	if (!reads(slave, "hi\n")) return fail("the line the slave read");
	if (!reads(master, "hi\r\n")) return fail("the echo the master read");

	/* What the slave writes goes through output processing. */
	if (!writes(slave, "out\n")) return fail("slave write");
	if (ioctl_count(master, NXU_FIONREAD) != 5U) return fail("FIONREAD on the master");
	if (!level_is(master, IOREADY_READ | IOREADY_WRITE)) return fail("master readiness with output waiting");
	if (!reads(master, "out\r\n")) return fail("the output the master read");

	/* A password prompt turns echo off; the line still arrives, and nothing comes back. */
	uint32_t lflag = NXU_ICANON | NXU_ISIG | NXU_IEXTEN;

	if (!set_modes(master, lflag, NXU_OPOST | NXU_ONLCR)) return fail("echo off through the master");
	if (!writes(master, "hunter2\r")) return fail("typing a password");
	if (!reads(slave, "hunter2\n")) return fail("the password the slave read");
	if (!nothing_to_read(master)) return fail("a password was echoed");
	if (!set_modes(master, lflag | NXU_ECHO | NXU_ECHOE | NXU_ECHOK | NXU_ECHOCTL, NXU_OPOST | NXU_ONLCR))
		return fail("echo back on");

	/* The emulator sets the window size; the shell reads it from the slave. */
	struct nxu_winsize set = {.ws_row = 24U, .ws_col = 80U, .ws_xpixel = 640U, .ws_ypixel = 384U};
	struct nxu_winsize got = {0};

	if (vfs_ioctl(&g_fd, master, NXU_TIOCSWINSZ, &set) != VFS_STATUS_OK) return fail("TIOCSWINSZ on the master");
	if (vfs_ioctl(&g_fd, slave, NXU_TIOCGWINSZ, &got) != VFS_STATUS_OK || got.ws_row != 24U || got.ws_col != 80U)
		return fail("window size on the slave");

	/* The master is not a terminal of anyone's: job control is not taken there. */
	int32_t group = 0;

	if (vfs_ioctl(&g_fd, master, NXU_TIOCGPGRP, &group) != VFS_STATUS_NOT_TTY) return fail("TIOCGPGRP on the master");

	return true;
}

/* A master sleeping in a read wakes for the slave's output. A pair of its own, its master opened to wait. */
static bool
test_blocked_master(void)
{
	uint32_t master;
	uint32_t slave;
	nxu_ptyname_t name;

	if (vfs_open(&g_fd, "/dev/ptmx", PTY_TEST_OPEN, &master) != VFS_STATUS_OK) return fail("open a waiting master");
	if (vfs_ioctl(&g_fd, master, NXU_TIOCPTYGNAME, &name) != VFS_STATUS_OK ||
		vfs_ioctl(&g_fd, master, NXU_TIOCPTYUNLK, 0) != VFS_STATUS_OK ||
		vfs_open(&g_fd, name.name, PTY_TEST_OPEN | VFS_OPEN_NONBLOCK, &slave) != VFS_STATUS_OK)
		return fail("the waiting master's slave");

	memset(&g_reader, 0, sizeof(g_reader));
	g_reader.descriptor = master;

	if (!start_thread(reader_thread, 0) || !wait_until(&g_reader.started)) return fail("start a master reader");
	if (!stays_clear(&g_reader.done)) return fail("the master read returned with nothing written");
	if (!writes(slave, "wake")) return fail("slave write to a sleeping master");
	if (!wait_until(&g_reader.done)) return fail("the sleeping master did not wake");
	if (g_reader.status != VFS_STATUS_OK || g_reader.count != 4ULL || memcmp(g_reader.buffer, "wake", 4U) != 0)
		return fail("what the woken master read");

	if (vfs_close(&g_fd, slave) != VFS_STATUS_OK || vfs_close(&g_fd, master) != VFS_STATUS_OK)
		return fail("close of the waiting master's pair");

	return true;
}

/* More output than the ring and the queue hold: the slave's writer waits for the master. */
static bool
test_stream(uint32_t master, uint32_t blocking_slave)
{
	uint8_t buffer[512];
	uint32_t received = 0U;
	uint64_t deadline = timer_get_microseconds() + PTY_TEST_TIMEOUT_US;

	if (!set_modes(master, 0U, 0U)) return fail("raw modes for the stream");

	for (uint32_t index = 0U; index < PTY_TEST_STREAM_SIZE; index++)
		g_stream[index] = stream_byte(index);

	memset(&g_writer, 0, sizeof(g_writer));
	g_writer.descriptor = blocking_slave;

	if (!start_thread(writer_thread, 0)) return fail("start the slave's writer");
	if (!stays_clear(&g_writer.done)) return fail("the stream's writer never waited for the master");

	while (received < PTY_TEST_STREAM_SIZE) {
		uint64_t count = 0ULL;
		vfs_status_t status = vfs_read(&g_fd, master, buffer, sizeof(buffer), &count);

		if (status == VFS_STATUS_WOULD_BLOCK) {
			if (timer_get_microseconds() > deadline) return fail("the stream stopped");

			(void)sched_yield();
			continue;
		}

		if (status != VFS_STATUS_OK || count == 0ULL) return fail("read of the stream");

		for (uint32_t index = 0U; index < (uint32_t)count; index++) {
			if (buffer[index] != stream_byte(received + index)) return fail("the stream arrived out of order");
		}

		received += (uint32_t)count;
	}

	if (!wait_until(&g_writer.done)) return fail("the stream's writer did not finish");
	if (g_writer.status != VFS_STATUS_OK || g_writer.count != PTY_TEST_STREAM_SIZE) return fail("the stream's write");

	return set_modes(master, NXU_ICANON | NXU_ISIG | NXU_IEXTEN | NXU_ECHO | NXU_ECHOE | NXU_ECHOK | NXU_ECHOCTL,
					 NXU_OPOST | NXU_ONLCR) ||
		   fail("canonical modes after the stream");
}

/* The slave's last words reach the master after its last close; then end of file. */
static bool
test_slave_close(uint32_t master, uint32_t *slave, uint32_t blocking_slave, const nxu_ptyname_t *name)
{
	if (!writes(*slave, "bye\n")) return fail("last words");
	if (vfs_close(&g_fd, *slave) != VFS_STATUS_OK || vfs_close(&g_fd, blocking_slave) != VFS_STATUS_OK)
		return fail("close of the slave");

	if (!level_is(master, IOREADY_READ | IOREADY_WRITE | IOREADY_HANGUP)) return fail("master readiness after the slave closed");
	if (!reads(master, "bye\r\n")) return fail("last words lost at the slave's close");
	if (!reads(master, "")) return fail("no end of file after the slave closed");

	/* The slave can be opened again, and the master is live again. */
	if (vfs_open(&g_fd, name->name, PTY_TEST_OPEN | VFS_OPEN_NONBLOCK, slave) != VFS_STATUS_OK)
		return fail("reopen of the slave");
	if (!nothing_to_read(master)) return fail("master not live again after the slave reopened");

	return true;
}

/* The master goes: the slave hangs up for good and the pair is freed with its last file. */
static bool
test_master_close(uint32_t master, uint32_t slave, const nxu_ptyname_t *name, uint32_t baseline,
				  uint32_t tty_baseline)
{
	vnode_t vnode;

	if (vfs_close(&g_fd, master) != VFS_STATUS_OK) return fail("close of the master");

	if (vfs_lookup(name->name, &vnode) != VFS_STATUS_NOT_FOUND) return fail("the slave kept its name");
	if (pty_count() != baseline + 1U) return fail("pair freed while its slave was open");
	if (!reads(slave, "")) return fail("slave read after the master closed is not end of file");

	uint64_t count = 0ULL;

	if (vfs_write(&g_fd, slave, "x", 1ULL, &count) != VFS_STATUS_IO_ERROR) return fail("slave write after hangup");
	if (vfs_close(&g_fd, slave) != VFS_STATUS_OK) return fail("last close of the slave");
	if (pty_count() != baseline || tty_count() != tty_baseline) return fail("pair not freed after its last file");

	return true;
}

bool
pty_test_run(void)
{
	uint32_t baseline = pty_count();
	uint32_t tty_baseline = tty_count();
	uint32_t devices = devfs_device_count();
	uint32_t master;
	uint32_t slave;
	uint32_t blocking_slave;
	nxu_ptyname_t name;

	filedesc_init(&g_fd);
	g_thread_count = 0U;

	bool ok = test_blocked_master() && pty_count() == baseline;

	if (!ok) return fail("the waiting master's pair was not freed");
	if (!test_make(&master, &slave, &blocking_slave, &name, baseline)) return false;

	ok = test_io(master, slave) && test_stream(master, blocking_slave) &&
		 test_slave_close(master, &slave, blocking_slave, &name) &&
		 test_master_close(master, slave, &name, baseline, tty_baseline);

	/* Only after a pass is every thread known to have finished. */
	if (!ok) return false;

	release_threads();
	if (devfs_device_count() != devices) return fail("device count after the test");

	return true;
}
