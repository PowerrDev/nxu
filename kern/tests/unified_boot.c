/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/unified_boot.c
 *
 * See kern/tests/unified_boot.h.
 */

#include <kern/tests/unified_boot.h>

#include <drivers/video/ui_service_host.h>
#include <kern/aqua/window_server.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_chime.h>
#include <kern/console/console.h>
#include <kern/ipc/ipc_init.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/tests/ipc_process_test.h>
#include <kern/tests/process_control_test.h>
#include <kern/tests/socket_process_test.h>
#include <kern/tests/sound_test.h>
#include <kern/tests/thread_process_test.h>

#include <stdbool.h>
#include <stdint.h>

#define UNIFIED_BOOT_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

/* Highest PID the summary looks for when it lists what is still running. */
#define UNIFIED_BOOT_PID_SCAN_MAX 64U

/* How long the tests wait for bootd to publish the bootstrap registry before they start anyway. */
#define UNIFIED_BOOT_REGISTRY_WAIT_US 10000000ULL

/* How long the summary watches the UI loop to see that it is still cycling. */
#define UNIFIED_BOOT_UI_WATCH_US 100000ULL

/* Arguments that keep bootd from starting the services that would each become the display server. */
#define UNIFIED_BOOT_NO_WINDOWSERVER "-no-windowserver"
#define UNIFIED_BOOT_NO_ABOUT_SEVOS "-no-about-sevos"

typedef enum {
	UNIFIED_PENDING,
	UNIFIED_RUNNING,
	UNIFIED_PASSED,
	UNIFIED_FAILED
} unified_state_t;

typedef struct {
	const char *name;
	bool (*run)(void);
	volatile unified_state_t state;
} unified_test_t;

/*
 * The tests that share this boot, in the order they run. Quick process tests
 * first; the sound test last because it plays in real time. Each is the same
 * test the standalone build runs, against the live system instead of a
 * private one (see the *_shared variants for the two that had to change).
 */
static unified_test_t g_unified_tests[] = {
	{ "ipc_process_test", ipc_process_test, UNIFIED_PENDING },
	{ "thread_process_test", thread_process_test, UNIFIED_PENDING },
	{ "process_control_test", process_control_test_shared, UNIFIED_PENDING },
	{ "socket_process_test", socket_process_test, UNIFIED_PENDING },
	{ "sound_test", sound_test_run_shared, UNIFIED_PENDING },
};

#define UNIFIED_TEST_COUNT (sizeof(g_unified_tests) / sizeof(g_unified_tests[0]))

static volatile bool g_unified_ui_failed;

static const char *unified_state_name(unified_state_t state)
{
	switch (state) {
	case UNIFIED_PENDING: return "not run";
	case UNIFIED_RUNNING: return "still running";
	case UNIFIED_PASSED: return "passed";
	case UNIFIED_FAILED: return "FAILED";
	}

	return "unknown";
}

static const char *unified_chime_name(void)
{
	switch (boot_chime_state()) {
	case BOOT_CHIME_IDLE: return "was not started";
	case BOOT_CHIME_PLAYING: return "is still playing";
	case BOOT_CHIME_PLAYED: return "played";
	case BOOT_CHIME_FAILED: return "played with underruns or did not play (see the boot_chime_play lines)";
	}

	return "unknown";
}

/* True when the UI loop's poll count moves while this thread waits: the session is alive, not just started. */
static bool unified_ui_alive(void)
{
	if (!ui_service_running()) return false;

	uint64_t polls = ui_service_poll_count();
	uint64_t deadline = timer_get_microseconds() + UNIFIED_BOOT_UI_WATCH_US;

	while (timer_get_microseconds() < deadline) {
		if (!sched_yield()) break;
	}

	return ui_service_running() && ui_service_poll_count() > polls;
}

/*
 * unified_boot_summary:
 *
 * One block, one fact per line: every test, then what else is running.
 * Returns whether everything passed and is alive.
 */
static bool unified_boot_summary(void)
{
	unsigned int passed = 0U;

	kputln("unified_boot_summary: begin");

	for (unsigned int index = 0U; index < UNIFIED_TEST_COUNT; index++) {
		unified_test_t *test = &g_unified_tests[index];

		kprintf("unified_boot_summary: %s %s\n", test->name, unified_state_name(test->state));
		if (test->state == UNIFIED_PASSED) passed++;
	}

	kprintf("unified_boot_summary: boot_chime %s\n", unified_chime_name());

	bool ui_alive = unified_ui_alive();

	if (ui_alive) {
		kprintf("unified_boot_summary: UIService desktop running (%llu event polls so far)\n", (unsigned long long)ui_service_poll_count());
	} else {
		kprintf("unified_boot_summary: UIService desktop FAILED: %s\n", g_unified_ui_failed ? "the session did not start" : "its event loop is not running");
	}

	/* Everything still alive in the process table is one of the boot's own daemons or a test's leftover. */
	for (proc_id_t pid = 1U; pid <= UNIFIED_BOOT_PID_SCAN_MAX; pid++) {
		proc_t proc = proc_find(pid);

		if (proc != 0) kprintf("unified_boot_summary: process %u (%s) running\n", (unsigned int)pid, proc->p_comm);
	}

	bool bootd_alive = proc_find(1U) != 0;

	if (!bootd_alive) kputln("unified_boot_summary: bootd FAILED: PID 1 is gone");

	bool all = passed == UNIFIED_TEST_COUNT && ui_alive && bootd_alive;

	if (all) {
		kprintf("unified_boot_summary: all %u test(s) passed, the UI session and bootd are running\n", (unsigned int)UNIFIED_TEST_COUNT);
	} else {
		kprintf("unified_boot_summary: FAILED: %u of %u test(s) passed\n", passed, (unsigned int)UNIFIED_TEST_COUNT);
	}

	return all;
}

/*
 * ipc_process_test spawns a bootd of its own when there is no bootstrap
 * registry yet, and a second bootd would replace the one this boot runs. So
 * the tests wait for the registry, which is the one thing they need from the
 * boot; the chime, the other services and the UI do not hold them up.
 */
static void unified_boot_wait_for_registry(void)
{
	uint64_t deadline = timer_get_microseconds() + UNIFIED_BOOT_REGISTRY_WAIT_US;

	while (ipc_bootstrap_registry_port() == IPC_PORT_NULL) {
		if (timer_get_microseconds() > deadline) {
			UNIFIED_BOOT_LOG("bootd did not publish the bootstrap registry in time: the tests start anyway\n");
			return;
		}

		if (!sched_yield()) return;
	}
}

static void unified_boot_thread(void *parameter)
{
	(void)parameter;

	unified_boot_wait_for_registry();

	/*
	 * The process tests start as soon as bootd is up, while the chime plays
	 * and the other services come up: they do not touch the sound device, and
	 * the scheduler charges the CPU they use against their own level, not the
	 * chime thread's. sound_test waits for the chime, which holds the
	 * exclusive device.
	 */
	for (unsigned int index = 0U; index < UNIFIED_TEST_COUNT; index++) {
		unified_test_t *test = &g_unified_tests[index];

		test->state = UNIFIED_RUNNING;
		UNIFIED_BOOT_LOG("starting %s\n", test->name);

		bool ok = test->run();

		test->state = ok ? UNIFIED_PASSED : UNIFIED_FAILED;

		if (ok) kprintf("%s: passed\n", test->name);
		else kprintf("%s: FAILED\n", test->name);
	}

	(void)unified_boot_summary();
}

void unified_boot_prepare(void)
{
	if (!boot_args_append(UNIFIED_BOOT_NO_WINDOWSERVER) || !boot_args_append(UNIFIED_BOOT_NO_ABOUT_SEVOS)) {
		UNIFIED_BOOT_LOG("the boot arguments are full: bootd will start services that claim the display\n");
		return;
	}

	UNIFIED_BOOT_LOG("the UI session owns the display: bootd starts without windowserver and about-sevos\n");
}

void unified_boot_run(void)
{
	thread_t thread;

	if (!kernel_thread_create(proc_task(proc_kernel()), unified_boot_thread, 0, &thread) || !sched_thread_start(thread)) {
		UNIFIED_BOOT_LOG("the test thread could not be started\n");
		return;
	}

	UNIFIED_BOOT_LOG("test thread started: %u test(s) run against the live system\n", (unsigned int)UNIFIED_TEST_COUNT);

	/* The session below never returns while it works, so it has to share the CPU or nothing else would run. */
	ui_service_set_cooperative(true);

	if (!windowserver_bootstrap()) {
		UNIFIED_BOOT_LOG("WindowServer bootstrap failed: no UI session\n");
		g_unified_ui_failed = true;
		return;
	}

	if (!ui_service_bootstrap()) {
		UNIFIED_BOOT_LOG("the UI session ended or did not start\n");
		g_unified_ui_failed = true;
	}
}

void unified_boot_run_ui_only(void)
{
	/* The session below never returns while it works, so it has to share the CPU or nothing else would run. */
	ui_service_set_cooperative(true);

	if (!windowserver_bootstrap()) {
		UNIFIED_BOOT_LOG("WindowServer bootstrap failed: no UI session\n");
		g_unified_ui_failed = true;
		return;
	}

	if (!ui_service_bootstrap()) {
		UNIFIED_BOOT_LOG("the UI session ended or did not start\n");
		g_unified_ui_failed = true;
	}
}
