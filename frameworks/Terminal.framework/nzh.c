/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * NXU's small interactive shell.
 *
 * On a terminal (Terminal.app gives it a pseudo-terminal) it is a Unix shell:
 * it edits the command line itself (line_edit.c), runs each command as a job
 * in a process group of its own on the same terminal, gives that group the
 * foreground while it runs and takes it back when it ends or stops. ^C and ^Z
 * reach the job from the line discipline, not from the shell; a stopped job
 * waits in the job table for `fg`. The shell ignores the job-control signals
 * itself, as interactive shells do.
 *
 * On anything else (a file, a socket) it reads plain lines and runs each
 * command with the same standard descriptors, in its own process group,
 * without job control: there is no terminal to give the foreground of.
 */
#include "shell.h"
#include "netutil.h"

#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <termios.h>

#define NZH_CHILD_MAX 100U
#define NZH_FORKBOMB_MAX_SECONDS 100U

static char g_directory[256] = "/Volumes/mainOS";
/* Optional prompt identity, read once from /etc/username and /etc/hostname.
 * Empty when unset, in which case the prompt is just "<dir> %". */
static char g_user[64];
static char g_host[64];
static volatile uint32_t g_children[NZH_CHILD_MAX];
static volatile bool g_interrupted;

/* Standard input is a terminal: jobs, the line editor, the terminal's own ^C. */
static bool g_tty;
static uint32_t g_shell_pgid;
static struct termios g_shell_termios;
static nzh_editor_t g_editor;

/* Jobs not in the foreground: stopped, or running after `bg`. A job is one process group of at most two processes. */
#define NZH_JOB_MAX 8U
#define NZH_JOB_PROCESSES 2U

typedef struct {
	uint32_t pgid;
	uint32_t pids[NZH_JOB_PROCESSES];
	uint32_t count;
	bool running; /* continued in the background by `bg` */
	char name[32];
} nzh_job_t;

static nzh_job_t g_jobs[NZH_JOB_MAX];

static void
nzh_signal(int signal)
{
	g_interrupted = signal == NXU_SIGINT;

	/* On a terminal the line discipline signals the foreground job itself. */
	for (uint32_t index = 0; !g_tty && index < NZH_CHILD_MAX; index++) {
		if (g_children[index] != 0) {
			(void)nxu_kill(g_children[index], signal);
		}
	}

	if (signal == NXU_SIGTERM) {
		(void)nxu_exit(0);
	}
}

static bool
nzh_join(char *output, uint32_t capacity, const char *directory, const char *name)
{
	uint64_t prefix = nxu_strlen(directory);
	uint64_t suffix = nxu_strlen(name);

	if (prefix + suffix + 2 > capacity) {
		return false;
	}

	memcpy(output, directory, prefix);
	output[prefix++] = '/';
	memcpy(output + prefix, name, suffix + 1);
	return true;
}

static bool
nzh_path(const char *name, char path[256])
{
	if (name[0] == '/') {
		if (nxu_strlen(name) >= 256) {
			return false;
		}

		memcpy(path, name, nxu_strlen(name) + 1);
		return true;
	}

	return nzh_join(path, 256, g_directory, name);
}

static bool
nzh_executable(const char *name, char path[256])
{
	for (const char *cursor = name; *cursor != 0; cursor++) {
		if (*cursor == '/') {
			return nzh_path(name, path);
		}
	}

	const char *directories[] = {"/Volumes/mainOS/System/Library/CoreServices", "/Volumes/mainOS/bin"};

	for (uint32_t index = 0; index < 2; index++) {
		nxu_stat_t info;

		if (nzh_join(path, 256, directories[index], name) && nxu_stat(path, &info) == 0 &&
			info.type == NXU_STAT_TYPE_REGULAR) {
			return true;
		}
	}

	return false;
}

/*
 * Start a job's process with the shell's descriptors 0-2. On a terminal it
 * joins group `pgid` (0: a new group named after it), and that group has the
 * foreground from the start, so the job never runs, even for a moment, as a
 * background one. Elsewhere it simply shares the shell's group.
 */
static int64_t
nzh_spawn_job(const char *path, const char *const *arguments, uint32_t pgid)
{
	char strings[NXU_EXEC_ARGV_BYTES];
	nxu_spawn_attr_t attr = {.size = sizeof(attr), .caps = (uint32_t)nxu_get_caps()};

	if (g_tty) {
		attr.flags = NXU_SPAWN_SETPGROUP | NXU_SPAWN_SETFG;
		attr.pgid = pgid;
	}

	if (!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size))
		return -NXU_SYS_E_INVALID_ARGUMENT;

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)arguments[0];
	attr.descriptor_count = 3;

	for (uint32_t descriptor = 0; descriptor < 3; descriptor++)
		attr.descriptors[descriptor] = (nxu_spawn_fd_t){.child = descriptor, .parent = descriptor};

	return nxu_spawn_attr(path, &attr);
}

static int64_t
nzh_launch(const char *name, const char *const *arguments, uint32_t pgid)
{
	char path[256];

	if (!nzh_executable(name, path)) {
		netutil_write("nzh: command not found: ");
		netutil_write(name);
		netutil_write("\n");
		return -1;
	}

	int64_t child = nzh_spawn_job(path, arguments, pgid);

	if (child < 0) {
		netutil_write("nzh: could not create child process\n");
	}

	return child;
}

/* The shell's terminal back: the foreground, and its settings if a job stopped while it had them changed. */
static void
nzh_take_terminal(bool restore)
{
	if (!g_tty) return;

	(void)tcsetpgrp(0, (int)g_shell_pgid);

	if (restore) {
		(void)tcsetattr(0, TCSADRAIN, &g_shell_termios);
	} else {
		(void)tcgetattr(0, &g_shell_termios);
	}
}

static void
nzh_job_report(uint32_t number, const char *state, const char *name)
{
	netutil_write("[");
	netutil_number(number + 1U);
	netutil_write("]  ");
	netutil_write(state);
	netutil_write("  ");
	netutil_write(name);
	netutil_write("\n");
}

/*
 * Wait for the foreground job in g_children[0..count): until each process
 * has exited, or one of them stops (^Z), in which case the job goes into the
 * table for `fg`. Either way the shell takes the terminal back, with the
 * settings it had if the job stopped or was killed (it may have been killed
 * with echo off); a job that simply exits keeps what it set, as `stty` must.
 */
static void
nzh_wait_job(uint32_t count, const char *name)
{
	bool stopped = false;
	bool killed = false;

	for (uint32_t slot = 0; slot < count; slot++) {
		if (g_children[slot] == 0) continue;

		uint64_t status = 0;
		int64_t result;

		do {
			result = nxu_wait_options(g_children[slot], &status, NXU_WAIT_UNTRACED);
		} while (result == -NXU_SYS_E_INTERRUPTED);

		if (result >= 0 && NXU_WAIT_WAS_STOPPED(status)) {
			stopped = true;
			continue;
		}

		g_children[slot] = 0;

		if (result >= 0 && NXU_EXIT_WAS_KILLED(status)) {
			uint32_t signal = (uint32_t)(status & 0xFFULL);

			killed = true;

			/* ^C ends a job quietly; any other signal is worth a word. */
			if (signal != NXU_SIGINT) {
				netutil_write("nzh: ");
				netutil_write(name);
				netutil_write(" killed by signal ");
				netutil_number(signal);
				netutil_write("\n");
			}
		} else if (result < 0 || status != 0) {
			netutil_write("nzh: command exited with status ");
			netutil_number(status);
			netutil_write("\n");
		}
	}

	g_interrupted = false;
	nzh_take_terminal(stopped || killed);

	if (!stopped) return;

	uint32_t number = 0;

	while (number < NZH_JOB_MAX && g_jobs[number].pgid != 0)
		number++;

	nzh_job_t job = {.pgid = (uint32_t)nxu_getpgid(g_children[0] != 0 ? g_children[0] : g_children[1])};

	for (uint32_t slot = 0; slot < count; slot++) {
		if (g_children[slot] != 0 && job.count < NZH_JOB_PROCESSES) job.pids[job.count++] = g_children[slot];

		g_children[slot] = 0;
	}

	uint64_t length = nxu_strlen(name);

	if (length >= sizeof(job.name)) length = sizeof(job.name) - 1;

	memcpy(job.name, name, length);
	job.name[length] = 0;

	netutil_write("\n");

	if (number == NZH_JOB_MAX) {
		/* No room to keep it: a job nobody can bring back would only linger. */
		(void)nxu_killpg(job.pgid, NXU_SIGKILL);
		(void)nxu_killpg(job.pgid, NXU_SIGCONT);
		netutil_write("nzh: too many stopped jobs; killed ");
		netutil_write(job.name);
		netutil_write("\n");
		return;
	}

	g_jobs[number] = job;
	nzh_job_report(number, "suspended", job.name);
}

static void nzh_reap_jobs(void);

/* `jobs`: every job not in the foreground, as they are now. */
static void
nzh_jobs(void)
{
	nzh_reap_jobs();

	for (uint32_t number = 0; number < NZH_JOB_MAX; number++) {
		if (g_jobs[number].pgid != 0)
			nzh_job_report(number, g_jobs[number].running ? "running" : "suspended", g_jobs[number].name);
	}
}

/*
 * Before each prompt: what became of the jobs in the background. A process
 * that exited is reaped, and a job none of whose processes is left is done;
 * one that stopped (a background job that read the terminal gets SIGTTIN)
 * is suspended again, for `fg`.
 */
static void
nzh_reap_jobs(void)
{
	for (uint32_t number = 0; number < NZH_JOB_MAX; number++) {
		nzh_job_t *job = &g_jobs[number];

		if (job->pgid == 0 || !job->running) continue;

		for (uint32_t index = 0; index < job->count;) {
			uint64_t status = 0;
			int64_t result = nxu_wait_options(job->pids[index], &status, NXU_WAIT_UNTRACED | NXU_WAIT_NOHANG);

			if (result >= 0 && NXU_WAIT_WAS_STOPPED(status)) {
				job->running = false;
				nzh_job_report(number, "suspended", job->name);
			}

			if (result == -NXU_SYS_E_AGAIN || (result >= 0 && NXU_WAIT_WAS_STOPPED(status))) {
				index++;
				continue;
			}

			/* Exited, or gone some other way: no longer part of the job. */
			job->pids[index] = job->pids[--job->count];
		}

		if (job->count == 0) {
			nzh_job_report(number, "done", job->name);
			job->pgid = 0;
		}
	}
}

/* The job `fg` or `bg` means: %n, n, or with none the most recent. */
static bool
nzh_pick_job(uint32_t count, const char *const *arguments, const char *command, uint32_t *number)
{
	*number = NZH_JOB_MAX;
	nzh_reap_jobs();

	if (!g_tty) {
		netutil_write(command);
		netutil_write(": no job control without a terminal\n");
		return false;
	}

	if (count > 1) {
		const char *text = arguments[1][0] == '%' ? arguments[1] + 1 : arguments[1];

		if (text[0] >= '1' && text[0] <= '9' && text[1] == 0) *number = (uint32_t)(text[0] - '1');
	} else {
		for (uint32_t candidate = 0; candidate < NZH_JOB_MAX; candidate++) {
			if (g_jobs[candidate].pgid != 0) *number = candidate;
		}
	}

	if (*number >= NZH_JOB_MAX || g_jobs[*number].pgid == 0) {
		netutil_write(command);
		netutil_write(": no such job\n");
		return false;
	}

	return true;
}

/* `bg [%n]`: a stopped job continues, in the background. */
static void
nzh_background(uint32_t count, const char *const *arguments)
{
	uint32_t number;

	if (!nzh_pick_job(count, arguments, "bg", &number)) return;

	nzh_job_t *job = &g_jobs[number];

	if (job->running) {
		netutil_write("bg: job already in background\n");
		return;
	}

	job->running = true;
	(void)nxu_killpg(job->pgid, NXU_SIGCONT);
	nzh_job_report(number, "continued", job->name);
}

/* `fg [%n]`: a stopped or background job back in the foreground, continued. */
static void
nzh_foreground(uint32_t count, const char *const *arguments)
{
	uint32_t number;

	if (!nzh_pick_job(count, arguments, "fg", &number)) return;

	nzh_job_t job = g_jobs[number];

	g_jobs[number].pgid = 0;
	netutil_write(job.name);
	netutil_write("\n");

	for (uint32_t slot = 0; slot < job.count; slot++)
		g_children[slot] = job.pids[slot];

	(void)tcsetpgrp(0, (int)job.pgid);
	(void)nxu_killpg(job.pgid, NXU_SIGCONT);
	nzh_wait_job(job.count, job.name);
}

static void
nzh_list(const char *name)
{
	char path[256];

	if (!nzh_path(name, path)) {
		return;
	}

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) {
		netutil_write("ls: cannot open directory\n");
		return;
	}

	nxu_dirent_t entry;

	while (nxu_readdir(descriptor, &entry) > 0) {
		netutil_write(entry.name);
		netutil_write("\n");
	}

	(void)nxu_close(descriptor);
}

static void
nzh_nvram(uint32_t count, const char *const *arguments)
{
	char value[512];
	int64_t length = nxu_get_boot_args(value, sizeof(value));
	if (length < 0) {
		netutil_write("nvram: could not read boot arguments\n");
		return;
	}
	if ((uint64_t)length >= sizeof(value)) length = (int64_t)sizeof(value) - 1;
	value[length] = '\0';

	if (count == 1 || nxu_streq(arguments[1], "boot-args")) {
		netutil_write("boot-args=");
		netutil_write(value);
		netutil_write("\n");
		return;
	}
	static const char prefix[] = "boot-args=";
	bool assignment = count == 2 && nxu_strlen(arguments[1]) > sizeof(prefix) - 1U;
	for (uint32_t index = 0U; assignment && index < sizeof(prefix) - 1U; index++) {
		if (arguments[1][index] != prefix[index]) assignment = false;
	}
	if (assignment) {
		netutil_write("nvram: this QEMU firmware exposes boot-args read-only. Set it on the next boot with make BOOT_ARGS=\"");
		netutil_write(arguments[1] + sizeof(prefix) - 1U);
		netutil_write("\". Example: make run BOOT_ARGS=\"-v\"\n");
		return;
	}
	netutil_write("usage: nvram [boot-args|boot-args=VALUE]\n");
}

static void
nzh_cat(const char *name)
{
	char path[256];

	if (!nzh_path(name, path)) {
		return;
	}

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) {
		netutil_write("cat: cannot open file\n");
		return;
	}

	char buffer[512];
	int64_t count;

	while ((count = nxu_read(descriptor, buffer, sizeof(buffer))) > 0) {
		(void)nxu_write(1, buffer, count);
	}

	(void)nxu_close(descriptor);
}

/*
 * Unified system log, macOS style. The kernel retains every console line --
 * kprintf/kverbosef milestones, syscall and scheduler diagnostics, and the
 * stdout/stderr of every daemon and GUI app that is not redirected elsewhere --
 * in a console history ring. logd drains that ring into system.log, so the file
 * is the complete record since boot and the ring is the live tail.
 */
#define NZH_SYSTEM_LOG "/Volumes/mainOS/var/log/system.log"
#define NZH_LOG_STREAM_POLL_US 50000ULL

/* Drain the live kernel console ring from its oldest retained byte to the end. */
static void
nzh_log_dump_ring(void)
{
	uint64_t cursor = 0ULL;
	char buffer[512];
	int64_t count;

	while ((count = nxu_klog_read(&cursor, buffer, sizeof(buffer))) > 0) {
		(void)nxu_write(1, buffer, (uint64_t)count);
	}
}

/* `log show`: the whole unified log since boot. */
static void
nzh_log_show(void)
{
	/*
	 * Prefer the persisted log drained by logd: it reaches back past whatever
	 * tail still fits in the kernel ring. Fall back to the live ring when logd
	 * is disabled (-no-logd / safe mode) and the file is missing or empty.
	 */
	int64_t descriptor = nxu_open(NZH_SYSTEM_LOG, NXU_O_READ);

	if (descriptor >= 0) {
		char buffer[512];
		int64_t count;
		int64_t total = 0;

		while ((count = nxu_read(descriptor, buffer, sizeof(buffer))) > 0) {
			(void)nxu_write(1, buffer, (uint64_t)count);
			total += count;
		}

		(void)nxu_close(descriptor);

		if (total > 0) {
			return;
		}
	}

	nzh_log_dump_ring();
}

/* `log stream`: follow new unified-log output until Ctrl-C. */
static void
nzh_log_stream(void)
{
	/*
	 * Snap the cursor to the current end so only fresh activity is shown; the
	 * kernel clamps an out-of-range cursor down to the live sequence number.
	 */
	uint64_t cursor = UINT64_MAX;
	char buffer[512];

	g_interrupted = false;
	netutil_write("log: streaming unified log -- press Ctrl-C to stop\n");

	while (!g_interrupted) {
		int64_t count = nxu_klog_read(&cursor, buffer, sizeof(buffer));

		if (count < 0) {
			netutil_write("log: kernel log read failed\n");
			return;
		}

		if (count == 0) {
			(void)nxu_sleep_us(NZH_LOG_STREAM_POLL_US);
			continue;
		}

		(void)nxu_write(1, buffer, (uint64_t)count);
	}
}

static void
nzh_log(uint32_t count, const char *const *arguments)
{
	const char *subcommand = count > 1 ? arguments[1] : "";

	if (nxu_streq(subcommand, "show")) {
		nzh_log_show();
	} else if (nxu_streq(subcommand, "stream")) {
		nzh_log_stream();
	} else {
		netutil_write(
			"usage: log show | log stream\n"
			"  show    print the unified system log (kernel, daemons, apps) since boot\n"
			"  stream  follow new unified-log output live until Ctrl-C\n");
	}
}

static void
nzh_remove(uint32_t count, const char *const *arguments)
{
	bool force = false;
	bool directories = false;
	uint32_t first = 1;

	for (; first < count; first++) {
		const char *option = arguments[first];

		if (nxu_streq(option, "--")) {
			first++;
			break;
		}

		if (option[0] != '-' || option[1] == 0) {
			break;
		}

		for (uint32_t index = 1; option[index] != 0; index++) {
			if (option[index] == 'f') {
				force = true;
			} else if (option[index] == 'd') {
				directories = true;
			} else {
				netutil_write("usage: rm [-f] [-d] [--] path ...\n");
				return;
			}
		}
	}

	if (first == count && !force) {
		netutil_write("usage: rm [-f] [-d] [--] path ...\n");
		return;
	}

	for (uint32_t index = first; index < count; index++) {
		char path[256];
		int64_t result = -NXU_SYS_E_INVALID_ARGUMENT;

		if (nzh_path(arguments[index], path)) {
			result = nxu_unlink(path);
			if (directories && result == -NXU_SYS_E_IS_DIRECTORY) {
				result = nxu_rmdir(path);
			}
		}

		if (result == 0 || (force && result == -NXU_SYS_E_NOT_FOUND)) {
			continue;
		}

		netutil_write("rm: ");
		netutil_write(arguments[index]);
		if (result == -NXU_SYS_E_NOT_FOUND) {
			netutil_write(": no such file or directory\n");
		} else if (result == -NXU_SYS_E_IS_DIRECTORY) {
			netutil_write(": is a directory (use -d for an empty directory)\n");
		} else if (result == -NXU_SYS_E_NOT_EMPTY) {
			netutil_write(": directory is not empty\n");
		} else if (result == -NXU_SYS_E_DENIED) {
			netutil_write(": permission denied\n");
		} else if (result == -NXU_SYS_E_READ_ONLY) {
			netutil_write(": read-only filesystem\n");
		} else {
			netutil_write(": cannot remove (error ");
			netutil_number((uint64_t)-result);
			netutil_write(")\n");
		}
	}
}

static bool
nzh_parse_bounded(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value)
{
	uint32_t result = 0;
	if (text == 0 || text[0] == 0) return false;

	for (uint32_t index = 0; text[index] != 0; index++) {
		if (text[index] < '0' || text[index] > '9') return false;
		uint32_t digit = (uint32_t)(text[index] - '0');
		if (result > (maximum - digit) / 10U) return false;
		result = result * 10U + digit;
	}

	if (result < minimum || result > maximum) return false;
	*value = result;
	return true;
}

static void
nzh_number_text(uint32_t value, char text[11])
{
	char reversed[10];
	uint32_t length = 0;
	do {
		reversed[length++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && length < sizeof(reversed));
	for (uint32_t index = 0; index < length; index++) text[index] = reversed[length - index - 1];
	text[length] = 0;
}

static uint64_t
nzh_forkbomb_worker(uint32_t seconds)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + (uint64_t)seconds * 1000000ULL;
	volatile uint64_t work = 1;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		for (uint32_t index = 0; index < 10000U; index++) work = work * 1664525U + 1013904223U;
		(void)nxu_yield();
	}
	return work == 0 ? 1 : 0;
}

static void
nzh_forkbomb(uint32_t count, const char *const *arguments)
{
	uint32_t workers = 8U, seconds = 5U;
	if ((count > 1 && !nzh_parse_bounded(arguments[1], 1U, NZH_CHILD_MAX, &workers)) ||
		(count > 2 && !nzh_parse_bounded(arguments[2], 1U, NZH_FORKBOMB_MAX_SECONDS, &seconds)) || count > 3) {
		netutil_write("usage: forkbomb [workers 1-100] [seconds 1-100]\n");
		return;
	}

	char seconds_text[11];
	nzh_number_text(seconds, seconds_text);
	uint32_t started = 0;
	netutil_write("forkbomb: starting ");
	netutil_number(workers);
	netutil_write(" bounded CPU worker(s) for ");
	netutil_number(seconds);
	netutil_write(" second(s); Ctrl-C stops them\n");

	for (uint32_t slot = 0; slot < workers; slot++) {
		const char *worker_arguments[] = {"nzh", "--forkbomb-worker", seconds_text, 0};

		/* One job: ^C from the line discipline stops every worker. */
		int64_t child = nzh_spawn_job("/Volumes/mainOS/System/Library/CoreServices/nzh", worker_arguments,
									  slot == 0 ? 0U : g_children[0]);

		if (child < 0) {
			netutil_write("forkbomb: process limit reached after ");
			netutil_number(started);
			netutil_write(" worker(s)\n");
			break;
		}
		g_children[slot] = (uint32_t)child;
		started++;
	}

	nzh_wait_job(started, "forkbomb");

	netutil_write("forkbomb: workers finished\n");
}

static bool
nzh_execute(uint32_t count, const char *const *arguments)
{
	const char *command = arguments[0];

	if (nxu_streq(command, "exit")) {
		return false;
	}

	if (nxu_streq(command, "help")) {
		netutil_write(
			"nzh: help commands echo pwd cd ls cat rm clear nvram log forkbomb jobs fg bg exit\n"
			"NVRAM: nvram [boot-args|boot-args=VALUE] (QEMU firmware boot-args are read-only)\n"
			"Logs: log show (unified system log since boot), log stream (follow live, Ctrl-C to stop)\n"
			"Files: rm [-f] [-d] [--] path ...; -d removes empty directories\n"
			"Stress: forkbomb [workers 1-100] [seconds 1-100] (bounded process/CPU load)\n"
			"Network: ping [-4|-6] [-c count] address; ifconfig\n"
			"Tests: ipctest, sockettest, terminaltest, threadtest, proctest, smptest, and installed executables\n"
			"Terminal: arrows edit/history, Tab completes commands, Ctrl-C interrupts, Ctrl-Z suspends (fg, bg resume)\n"
			"Quoted arguments and escapes supported; pipelines and scripting are not implemented.\n");
	} else if (nxu_streq(command, "echo")) {
		for (uint32_t index = 1; index < count; index++) {
			if (index != 1) {
				netutil_write(" ");
			}
			netutil_write(arguments[index]);
		}
		netutil_write("\n");
	} else if (nxu_streq(command, "pwd")) {
		netutil_write(g_directory);
		netutil_write("\n");
	} else if (nxu_streq(command, "cd")) {
		char path[256];
		nxu_stat_t info;

		if (nzh_path(count > 1 ? arguments[1] : "/Volumes/mainOS", path) && nxu_stat(path, &info) == 0 &&
			info.type == NXU_STAT_TYPE_DIRECTORY) {
			memcpy(g_directory, path, nxu_strlen(path) + 1);
		} else {
			netutil_write("cd: directory not found\n");
		}
	} else if (nxu_streq(command, "ls")) {
		nzh_list(count > 1 ? arguments[1] : ".");
	} else if (nxu_streq(command, "cat")) {
		if (count == 2) {
			nzh_cat(arguments[1]);
		} else {
			netutil_write("usage: cat path\n");
		}
	} else if (nxu_streq(command, "rm")) {
		nzh_remove(count, arguments);
	} else if (nxu_streq(command, "clear")) {
		netutil_write("\033[2J\033[H");
	} else if (nxu_streq(command, "jobs")) {
		nzh_jobs();
	} else if (nxu_streq(command, "fg")) {
		nzh_foreground(count, arguments);
	} else if (nxu_streq(command, "bg")) {
		nzh_background(count, arguments);
	} else if (nxu_streq(command, "forkbomb")) {
		nzh_forkbomb(count, arguments);
	} else if (nxu_streq(command, "nvram")) {
		nzh_nvram(count, arguments);
	} else if (nxu_streq(command, "log")) {
		nzh_log(count, arguments);
	} else if (nxu_streq(command, "commands")) {
		nzh_list("/Volumes/mainOS/System/Library/CoreServices");
	} else if (nxu_streq(command, "ipctest") || nxu_streq(command, "sockettest")) {
		const char *server = nxu_streq(command, "ipctest") ? "ipctest_a" : "sockettest_server";
		const char *client = nxu_streq(command, "ipctest") ? "ipctest_b" : "sockettest_client";
		const char *server_args[] = {server, 0};
		const char *client_args[] = {client, 0};
		int64_t first = nzh_launch(server, server_args, 0);

		if (first > 0) {
			g_children[0] = first;
			int64_t second = nzh_launch(client, client_args, (uint32_t)first);
			if (second > 0) {
				g_children[1] = second;
			} else {
				(void)nxu_kill(first, NXU_SIGTERM);
			}

			nzh_wait_job(2, command);
		}
	} else {
		int64_t child = nzh_launch(command, arguments, 0);

		if (child > 0) {
			g_children[0] = child;
			nzh_wait_job(1, command);
		}
	}

	return true;
}

/* Read the first whitespace-delimited token of `path` into `buffer` (empty on
 * any failure). Used for the optional /etc/username and /etc/hostname. */
static void
nzh_read_config(const char *path, char *buffer, uint32_t capacity)
{
	buffer[0] = 0;

	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) {
		return;
	}

	char raw[64];
	int64_t count = nxu_read(descriptor, raw, sizeof(raw));
	(void)nxu_close(descriptor);

	if (count <= 0) {
		return;
	}

	uint32_t length = 0;
	for (int64_t index = 0; index < count && length + 1 < capacity; index++) {
		char character = raw[index];
		if (character == '\n' || character == '\r' || character == ' ' || character == '\t' || character == 0) {
			break;
		}
		buffer[length++] = character;
	}
	buffer[length] = 0;
}

static void
nzh_append(char *buffer, uint32_t capacity, uint32_t *length, const char *text)
{
	while (*text != 0 && *length + 1 < capacity)
		buffer[(*length)++] = *text++;

	buffer[*length] = 0;
}

/* "user@host dir % " when an identity is set, otherwise "dir % ". `dir` is the
 * last component of the current directory. */
static void
nzh_format_prompt(char *prompt, uint32_t capacity)
{
	const char *dir = g_directory;
	uint32_t length = 0;

	for (const char *cursor = g_directory; *cursor != 0; cursor++) {
		if (*cursor == '/' && *(cursor + 1) != 0) {
			dir = cursor + 1;
		}
	}

	prompt[0] = 0;

	if (g_user[0] != 0 && g_host[0] != 0) {
		nzh_append(prompt, capacity, &length, g_user);
		nzh_append(prompt, capacity, &length, "@");
		nzh_append(prompt, capacity, &length, g_host);
		nzh_append(prompt, capacity, &length, " ");
	}

	nzh_append(prompt, capacity, &length, dir);
	nzh_append(prompt, capacity, &length, " % ");
}

/* The shell's own signals on a terminal: ^C abandons the line being typed; the job-control ones are the jobs'. */
static void
nzh_setup_terminal(void)
{
	g_tty = isatty(0) != 0;

	if (!g_tty) return;

	g_shell_pgid = (uint32_t)nxu_getpgid(0);
	(void)nxu_signal(NXU_SIGQUIT, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGTSTP, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGTTIN, (void (*)(int))NXU_SIG_IGN);
	(void)nxu_signal(NXU_SIGTTOU, (void (*)(int))NXU_SIG_IGN);
	(void)tcsetpgrp(0, (int)g_shell_pgid);
	(void)tcgetattr(0, &g_shell_termios);
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 3 && nxu_streq(argv[1], "--forkbomb-worker")) {
		uint32_t seconds;
		if (!nzh_parse_bounded(argv[2], 1U, NZH_FORKBOMB_MAX_SECONDS, &seconds)) return 2;
		return (int)nzh_forkbomb_worker(seconds);
	}

	(void)nxu_signal(NXU_SIGINT, nzh_signal);
	(void)nxu_signal(NXU_SIGTERM, nzh_signal);
	nzh_setup_terminal();
	nzh_read_config("/Volumes/mainOS/etc/username", g_user, sizeof(g_user));
	nzh_read_config("/Volumes/mainOS/etc/hostname", g_host, sizeof(g_host));
	netutil_write("NXU shell (nzh) - type help\n");

	for (;;) {
		char prompt[160];
		char line[NZH_LINE_MAX];

		nzh_format_prompt(prompt, sizeof(prompt));
		nzh_reap_jobs();

		/* A terminal gets the line editor; anything else is read a line at a time (nzh_edit_line does both). */
		int result = nzh_edit_line(&g_editor, prompt, line, sizeof(line), &g_interrupted);

		if (result == NZH_EDIT_EOF) return 0;
		if (result == NZH_EDIT_INTERRUPTED) continue;

		line[result] = 0;
		const char *arguments[NZH_ARGUMENT_MAX + 1];
		int count = nzh_parse(line, arguments);

		if (count < 0) {
			netutil_write("nzh: invalid quoting, unsupported operator, or command too long\n");
		} else if (count > 0 && !nzh_execute(count, arguments)) {
			return 0;
		}
	}
}
