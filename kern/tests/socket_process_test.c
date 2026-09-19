/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/socket_process_test.c
 *
 * Spawns two real processes -- sockettest_server and sockettest_client
 * (frameworks/BootDaemons.framework/sockettest_server.c / _client.c) --
 * that find each other purely through kern/ipc/socket.h's named registry
 * (NXU_SYS_SOCKET_LISTEN/_CONNECT/_ACCEPT), then stream a payload larger
 * than NXPC's old 4096-byte inline limit back and forth over ordinary
 * nxu_read/nxu_write on the resulting fds. This is the end-to-end proof
 * that the new socket primitive is a real blocking byte stream across
 * process boundaries, not a relabeled bounded message.
 */

#include <kern/tests/socket_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define SOCKET_PROCESS_TEST_WAIT_ITERATIONS 2000000U

static bool
socket_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < SOCKET_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
socket_process_test(void)
{
	proc_t proc_server = 0;
	proc_t proc_client = 0;

	loader_status_t status_server = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/sockettest_server", "sockettest_server", &proc_server);
	if (status_server != LOADER_STATUS_OK) {
		kputs("socket_process_test: spawning sockettest_server failed: ");
		kputln(loader_status_name(status_server));
		return false;
	}

	loader_status_t status_client = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/sockettest_client", "sockettest_client", &proc_client);
	if (status_client != LOADER_STATUS_OK) {
		kputs("socket_process_test: spawning sockettest_client failed: ");
		kputln(loader_status_name(status_client));
		return false;
	}

	uint64_t exit_server = 0ULL;
	uint64_t exit_client = 0ULL;

	if (!socket_process_test_wait(proc_kernel(), proc_server->p_ident.pid, &exit_server)) {
		kputln("socket_process_test: sockettest_server did not exit in time");
		return false;
	}

	if (!socket_process_test_wait(proc_kernel(), proc_client->p_ident.pid, &exit_client)) {
		kputln("socket_process_test: sockettest_client did not exit in time");
		return false;
	}

	if (exit_server != 0ULL || exit_client != 0ULL) {
		kprintf("socket_process_test: nonzero exit status (server=%llu client=%llu)\n", (unsigned long long)exit_server, (unsigned long long)exit_client);
		return false;
	}

	kputln("socket_process_test: two-process streaming socket round trip passed (listen, connect, accept, stream > 4096 bytes, reply, reap)");
	return true;
}
