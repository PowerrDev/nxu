#include <nxu/thread.h>

/* Guest checks exercise scheduler waits and real IPC on either architecture. */
static void
event_wait_sender(void *argument)
{
	(void)nxu_sleep_us(20000);

	uint8_t byte = 42;

	(void)nxu_ipc_send((uint32_t)(uintptr_t)argument, &byte, 1, 0);
	(void)nxu_thread_exit(0);
}

static uint64_t
event_wait_kernel_cpu(void)
{
	static nxu_stats_process_t processes[64];

	nxu_stats_request_t request = {
		sizeof(request),
		NXU_STATS_VERSION,
		NXU_STATS_PROCESS,
		sizeof(processes[0]),
		(uint64_t)(uintptr_t)processes,
		64,
		0,
		0,
		0,
	};
	if (nxu_statistics(&request) < 0) {
		return UINT64_MAX;
	}

	for (uint32_t i = 0; i < request.count; i++) {
		if (!processes[i].pid) {
			return processes[i].cpu_us;
		}
	}

	return UINT64_MAX;
}

static bool
event_wait_probe(void)
{
	int64_t port = nxu_ipc_port_allocate();

	if (port <= 0) {
		return false;
	}

	uint32_t sequence = (uint32_t)nxu_event_wait(0, 0);
	uint8_t byte = 7, received = 0;

	if (nxu_ipc_send((uint32_t)port, &byte, 1, 0) < 0) {
		return false;
	}

	uint64_t start = (uint64_t)nxu_uptime_us();

	/* Enqueue between the condition check and sleep must not be lost. */
	if (nxu_event_wait(2000000, sequence) < 0 || (uint64_t)nxu_uptime_us() - start >= 1000000) {
		return false;
	}

	if (nxu_ipc_receive_timed((uint32_t)port, &received, 1, 0, 0, 2000000) != 1 || received != byte) {
		return false;
	}

	start = (uint64_t)nxu_uptime_us();
	if (nxu_ipc_receive_timed((uint32_t)port, &received, 1, 0, 0, 20000) != -NXU_SYS_E_AGAIN ||
		(uint64_t)nxu_uptime_us() - start < 20000) {
		return false;
	}

	if (nxu_thread_spawn(event_wait_sender, (void *)(uintptr_t)port, 16384) < 0) {
		return false;
	}

	start = (uint64_t)nxu_uptime_us();
	if (nxu_ipc_receive_timed((uint32_t)port, &received, 1, 0, 0, 2000000) != 1 || received != 42 ||
		(uint64_t)nxu_uptime_us() - start >= 1000000) {
		return false;
	}

	(void)nxu_ipc_port_deallocate((uint32_t)port);

	uint64_t before = event_wait_kernel_cpu();

	start = (uint64_t)nxu_uptime_us();
	(void)nxu_sleep_us(500000);

	uint64_t after = event_wait_kernel_cpu();
	uint64_t elapsed = (uint64_t)nxu_uptime_us() - start;

	if (before == UINT64_MAX || after == UINT64_MAX || !elapsed) {
		return false;
	}

	uint64_t used = after > before ? after - before : 0;
	uint32_t permille = (uint32_t)(used * 1000 / elapsed);

	if (permille >= 500) {
		return false;
	}

	char usage[] = "event-wait-probe: kernel idle CPU 00.0%\n";

	usage[sizeof("event-wait-probe: kernel idle CPU ") - 1] = '0' + permille / 100;
	usage[sizeof("event-wait-probe: kernel idle CPU ")] = '0' + (permille / 10) % 10;
	usage[sizeof("event-wait-probe: kernel idle CPU ") + 2] = '0' + permille % 10;
	(void)nxu_write(1, usage, sizeof(usage) - 1);

	static const char passed[] = "event-wait-probe: lost-wakeup, timeout and IPC wake PASS\n";

	(void)nxu_write(1, passed, sizeof(passed) - 1);
	return true;
}
