/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/fault_process_test.c
 *
 * Spawns faulttest (frameworks/BootDaemons.framework/faulttest.c) once per
 * fault mode and checks that the kernel kills exactly that process with the
 * right signal and keeps running -- the end-to-end proof that a crashing
 * user process no longer takes the kernel down with it. Every spawn after
 * the first is also proof the kernel survived the previous fault.
 */

#include <kern/tests/fault_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall_defs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>

#define FAULT_PROCESS_TEST_WAIT_ITERATIONS 2000000U
#define FAULT_PROCESS_TEST_MODE_PATH "/fault-mode"
#define FAULT_PROCESS_TEST_CONTROL_STATUS 42ULL

typedef struct {
	char mode;
	uint64_t expected_status;
	const char *description;
} fault_process_case_t;

static const fault_process_case_t g_cases[] = {
	{ '0', NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV), "null pointer read" },
	{ '1', NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV), "write to read-only text" },
	{ '2', NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV), "jump into non-executable data" },
	{ '3', NXU_EXIT_KILLED_SIGNAL(NXU_SIGILL), "undefined instruction" },
	{ '4', NXU_EXIT_KILLED_SIGNAL(NXU_SIGTRAP), "breakpoint" },
	{ '5', NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV), "kernel address from EL0" },
	{ '6', NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV), "fault on a secondary thread" },
	{ '7', FAULT_PROCESS_TEST_CONTROL_STATUS, "control: clean exit" }
};

static bool
fault_process_test_set_mode(char mode)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;

	vfs_status_t status = vfs_open(
		filedesc,
		FAULT_PROCESS_TEST_MODE_PATH,
		VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	uint64_t written;
	status = vfs_write(filedesc, descriptor, &mode, 1ULL, &written);
	bool wrote = status == VFS_STATUS_OK && written == 1ULL;

	return vfs_close(filedesc, descriptor) == VFS_STATUS_OK && wrote;
}

static bool
fault_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < FAULT_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
fault_process_test(void)
{
	for (uint32_t index = 0U; index < sizeof(g_cases) / sizeof(g_cases[0]); index++) {
		const fault_process_case_t *test_case = &g_cases[index];

		if (!fault_process_test_set_mode(test_case->mode)) {
			kputln("fault_process_test: could not write " FAULT_PROCESS_TEST_MODE_PATH);
			return false;
		}

		proc_t proc = 0;
		loader_status_t loaded = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/faulttest", "faulttest", &proc);
		if (loaded != LOADER_STATUS_OK) {
			kputs("fault_process_test: spawning faulttest failed: ");
			kputln(loader_status_name(loaded));
			return false;
		}

		uint64_t exit_status = 0ULL;
		if (!fault_process_test_wait(proc_kernel(), proc->p_ident.pid, &exit_status)) {
			kprintf("fault_process_test: %s: faulttest did not exit in time\n", test_case->description);
			return false;
		}

		if (exit_status != test_case->expected_status) {
			kprintf(
				"fault_process_test: %s: exit status 0x%llx, expected 0x%llx\n",
				test_case->description,
				(unsigned long long)exit_status,
				(unsigned long long)test_case->expected_status
			);
			return false;
		}

		kprintf("fault_process_test: %s: exit status 0x%llx as expected\n", test_case->description, (unsigned long long)exit_status);
	}

	(void)vfs_unlink(FAULT_PROCESS_TEST_MODE_PATH);

	kputln("fault_process_test: every user fault killed only its own process and the kernel kept running");
	return true;
}
