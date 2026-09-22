#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/thread.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * SMP userland test: spawned by kern/tests/smp_user_test.c on a machine with
 * several CPUs. Everything here runs at EL0 -- real processes and threads that
 * really are placed on different CPUs -- and proves two things:
 *
 *   1. They run at the same time. Most cases are handshakes that cannot finish
 *      unless the parties execute simultaneously: a token passed around a ring
 *      of spinning threads needs every one of them on a CPU at once, and takes a
 *      scheduling quantum per hand-off (40 ms) if they have to share one CPU, so
 *      completing in a fraction of a second is only possible on different CPUs.
 *      The CPU each party ran on is read with nxu_getcpu() and reported.
 *   2. They stay correct while doing it: address spaces stay isolated across
 *      migrations, concurrent page faults on one page leave one mapping,
 *      shared memory and IPC work across CPUs, and killing or exiting a process
 *      whose threads run on other CPUs cleans up.
 *
 * main returns 0 when every case passes, otherwise the number of the first case
 * that failed. The cases print one line each.
 */

#define MAX_CPUS 8U
#define PAGE 4096ULL

/* A case that has not finished after this long has hung. */
#define CASE_TIMEOUT_US 20000000ULL

/* The ring must finish far quicker than one scheduling quantum per hand-off allows on a single CPU. */
#define RING_TIME_LIMIT_US 6000000ULL

static uint32_t g_online;
static uint32_t g_online_count;

/* ---- small helpers -------------------------------------------------------- */

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static void
say_number(uint64_t value)
{
	char digits[24];
	uint32_t length = 0U;

	if (value == 0ULL) digits[length++] = '0';

	while (value != 0ULL) {
		digits[length++] = (char)('0' + (value % 10ULL));
		value /= 10ULL;
	}

	char text[24];

	for (uint32_t index = 0U; index < length; index++) text[index] = digits[length - 1U - index];

	(void)nxu_write(1ULL, text, length);
}

static int
fail(int number, const char *what)
{
	say("smptest: case ");
	say_number((uint64_t)number);
	say(" FAILED: ");
	say(what);
	say("\n");
	return number;
}

static uint64_t
now_us(void)
{
	return (uint64_t)nxu_uptime_us();
}

static uint32_t
popcount(uint32_t value)
{
	uint32_t count = 0U;

	while (value != 0U) {
		count += value & 1U;
		value >>= 1U;
	}

	return count;
}

static void
note_cpu(volatile uint32_t *mask)
{
	int64_t cpu = nxu_getcpu();

	if (cpu >= 0 && cpu < (int64_t)MAX_CPUS) (void)__atomic_fetch_or(mask, 1U << (uint32_t)cpu, __ATOMIC_ACQ_REL);
}

/* The nth online CPU (0-based), wrapping. */
static uint32_t
nth_online(uint32_t n)
{
	uint32_t want = n % g_online_count;

	for (uint32_t cpu = 0U; cpu < MAX_CPUS; cpu++) {
		if ((g_online & (1U << cpu)) == 0U) continue;
		if (want == 0U) return cpu;
		want--;
	}

	return 0U;
}

static uint32_t
probe_online_cpus(void)
{
	uint32_t mask = 0U;

	for (uint32_t cpu = 0U; cpu < MAX_CPUS; cpu++) {
		if (nxu_setaffinity(1ULL << cpu) == 0) mask |= 1U << cpu;
	}

	(void)nxu_setaffinity(0xFFULL);
	return mask;
}

/* Wait for *value to reach at least `target`; false if it takes implausibly long. */
static bool
wait_at_least(volatile uint32_t *value, uint32_t target)
{
	uint64_t deadline = now_us() + CASE_TIMEOUT_US;
	uint32_t spins = 0U;

	while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target) {
		if ((++spins & 0xFFFU) == 0U && now_us() > deadline) return false;
		__asm__ volatile("yield");
	}

	return true;
}

/* Shared between processes: created before fork, inherited by every child. */
typedef struct {
	volatile uint32_t ring_token;
	volatile uint32_t ring_mask;
	volatile uint32_t ring_failed;
	volatile uint64_t counter;
	volatile uint32_t block_ready;
	volatile uint64_t block[64];
	volatile uint32_t ready;
	volatile uint32_t stop;
	volatile uint32_t spin_mask;
} shared_t;

static shared_t *g_shared;

static bool
shared_create(void)
{
	int64_t id = nxu_shm_create(sizeof(shared_t));

	if (id <= 0) return false;

	int64_t base = nxu_shm_map((uint64_t)id);

	if (base <= 0) return false;

	g_shared = (shared_t *)base;
	return true;
}

static void
shared_reset(void)
{
	g_shared->ring_token = 0U;
	g_shared->ring_mask = 0U;
	g_shared->ring_failed = 0U;
	g_shared->counter = 0ULL;
	g_shared->block_ready = 0U;
	g_shared->ready = 0U;
	g_shared->stop = 0U;
	g_shared->spin_mask = 0U;
	for (uint32_t index = 0U; index < 64U; index++) g_shared->block[index] = 0ULL;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ---- cases 1 and 2: a token ring around spinning threads / processes ------ */

#define RING_ROUNDS 200U

/*
 * One participant of a ring of `members`: waits for the token to be its index,
 * hands it on, RING_ROUNDS times, without ever sleeping or yielding. Every one of
 * them must be executing at the same time for this to complete quickly.
 */
static bool
ring_participate(volatile uint32_t *token, volatile uint32_t *mask, uint32_t index, uint32_t members)
{
	uint64_t deadline = now_us() + CASE_TIMEOUT_US;

	for (uint32_t round = 0U; round < RING_ROUNDS; round++) {
		uint32_t spins = 0U;

		while (__atomic_load_n(token, __ATOMIC_ACQUIRE) != index) {
			if ((++spins & 0x3FFU) == 0U) {
				note_cpu(mask);
				if (now_us() > deadline) return false;
			}
		}

		note_cpu(mask);
		__atomic_store_n(token, (index + 1U) % members, __ATOMIC_RELEASE);
	}

	return true;
}

static volatile uint32_t g_ring_token;
static volatile uint32_t g_ring_mask;
static volatile uint32_t g_ring_done;
static volatile uint32_t g_ring_failed;

static void
ring_thread(void *argument)
{
	if (!ring_participate(&g_ring_token, &g_ring_mask, (uint32_t)(uint64_t)argument, 4U)) {
		(void)__atomic_fetch_add(&g_ring_failed, 1U, __ATOMIC_ACQ_REL);
	}

	(void)__atomic_fetch_add(&g_ring_done, 1U, __ATOMIC_ACQ_REL);
	(void)nxu_thread_exit(0ULL);
}

static int
case_thread_ring(void)
{
	g_ring_token = 0U;
	g_ring_mask = 0U;
	g_ring_done = 0U;
	g_ring_failed = 0U;

	uint64_t started = now_us();

	for (uint64_t index = 1ULL; index < 4ULL; index++) {
		if (nxu_thread_spawn(ring_thread, (void *)index, 16ULL * 1024ULL) <= 0) return fail(1, "thread_spawn");
	}

	if (!ring_participate(&g_ring_token, &g_ring_mask, 0U, 4U)) return fail(1, "the ring stalled");
	if (!wait_at_least(&g_ring_done, 3U)) return fail(1, "threads did not finish");

	uint64_t elapsed = now_us() - started;
	uint32_t cpus = popcount(g_ring_mask);

	say("smptest: 1 threads of one process: ring of 4 done in ");
	say_number(elapsed);
	say(" us, ran on ");
	say_number(cpus);
	say(" CPUs (mask ");
	say_number(g_ring_mask);
	say(")\n");

	if (g_ring_failed != 0U) return fail(1, "a ring participant timed out");
	if (elapsed > RING_TIME_LIMIT_US) return fail(1, "the ring was too slow to have run in parallel");
	if (cpus < 3U && g_online_count >= 4U) return fail(1, "threads of one process did not spread over the CPUs");
	if (cpus < 2U) return fail(1, "threads of one process ran on one CPU");
	return 0;
}

static int
case_process_ring(void)
{
	shared_reset();

	uint64_t started = now_us();
	int64_t children[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		int64_t pid = nxu_fork();

		if (pid < 0) return fail(2, "fork");

		if (pid == 0) {
			bool ok = ring_participate(&g_shared->ring_token, &g_shared->ring_mask, index + 1U, 4U);

			nxu_exit(ok ? 0ULL : 1ULL);
		}

		children[index] = pid;
	}

	bool ok = ring_participate(&g_shared->ring_token, &g_shared->ring_mask, 0U, 4U);

	for (uint32_t index = 0U; index < 3U; index++) {
		uint64_t status = 0ULL;

		if (nxu_wait((uint64_t)children[index], &status) != children[index] || status != 0ULL) ok = false;
	}

	uint64_t elapsed = now_us() - started;
	uint32_t cpus = popcount(g_shared->ring_mask);

	say("smptest: 2 processes: ring of 4 done in ");
	say_number(elapsed);
	say(" us, ran on ");
	say_number(cpus);
	say(" CPUs (mask ");
	say_number(g_shared->ring_mask);
	say(")\n");

	if (!ok) return fail(2, "a ring participant failed");
	if (elapsed > RING_TIME_LIMIT_US) return fail(2, "the ring was too slow to have run in parallel");
	if (cpus < 3U && g_online_count >= 4U) return fail(2, "processes did not spread over the CPUs");
	if (cpus < 2U) return fail(2, "processes ran on one CPU");
	return 0;
}

/* ---- case 3: address spaces stay isolated across preemption and migration - */

#define ISOLATION_PAGES 16U
#define ISOLATION_ITERATIONS 120U

static int
isolation_child(uint32_t member)
{
	uint64_t pid = (uint64_t)nxu_getpid();
	int64_t base = nxu_mmap(ISOLATION_PAGES * PAGE, NXU_MMAP_PROT_READ_WRITE);

	if (base <= 0) return 1;

	volatile uint64_t *words = (volatile uint64_t *)base;
	uint32_t count = (uint32_t)(ISOLATION_PAGES * PAGE / 8ULL);

	for (uint32_t iteration = 0U; iteration < ISOLATION_ITERATIONS; iteration++) {
		uint64_t stamp = (pid << 40U) | ((uint64_t)iteration << 20U);

		/* Every process uses the same virtual addresses with different contents. */
		for (uint32_t index = 0U; index < count; index++) words[index] = stamp ^ index;

		uint32_t target = nth_online(iteration + member);

		if (nxu_setaffinity(1ULL << target) != 0) return 2;
		if ((uint32_t)nxu_getcpu() != target) return 3;

		for (uint32_t index = 0U; index < count; index++) {
			if (words[index] != (stamp ^ index)) return 4;
		}
	}

	(void)nxu_munmap((uint64_t)base, ISOLATION_PAGES * PAGE);
	return 0;
}

static int
case_isolation(void)
{
	int64_t children[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		int64_t pid = nxu_fork();

		if (pid < 0) return fail(3, "fork");

		if (pid == 0) nxu_exit((uint64_t)isolation_child(index + 1U));

		children[index] = pid;
	}

	int own = isolation_child(0U);

	(void)nxu_setaffinity(0xFFULL);

	bool ok = own == 0;

	for (uint32_t index = 0U; index < 3U; index++) {
		uint64_t status = 0ULL;

		if (nxu_wait((uint64_t)children[index], &status) != children[index] || status != 0ULL) ok = false;
	}

	say("smptest: 3 address-space isolation across ");
	say_number(4ULL * ISOLATION_ITERATIONS);
	say(" migrations: ");
	say(ok ? "ok\n" : "FAILED\n");

	return ok ? 0 : fail(3, "a process saw another's memory, or lost its own, across a migration");
}

/* ---- case 4: many threads fault on the same fresh page at the same time --- */

#define FAULT_PAGES 96U
#define FAULT_THREADS 4U

static volatile uint64_t *g_fault_base;
static volatile uint64_t *g_fault_read_base;
static volatile uint32_t g_fault_arrive[FAULT_PAGES];
static volatile uint32_t g_fault_read_arrive[FAULT_PAGES];
static volatile uint32_t g_fault_done;
static volatile uint32_t g_fault_bad;

static void
fault_run(uint32_t index)
{
	uint64_t deadline = now_us() + CASE_TIMEOUT_US;

	/* Writes: all threads arrive at a page, then all write to it at once. */
	for (uint32_t page = 0U; page < FAULT_PAGES; page++) {
		(void)__atomic_fetch_add(&g_fault_arrive[page], 1U, __ATOMIC_ACQ_REL);

		uint32_t spins = 0U;

		while (__atomic_load_n(&g_fault_arrive[page], __ATOMIC_ACQUIRE) < FAULT_THREADS) {
			if ((++spins & 0xFFFU) == 0U && now_us() > deadline) {
				(void)__atomic_fetch_add(&g_fault_bad, 1U, __ATOMIC_ACQ_REL);
				return;
			}
		}

		g_fault_base[(uint64_t)page * 512ULL + index] = 0x1000ULL * (index + 1U) + page;
	}

	/* Reads of untouched pages, then a write to the same page: read-mapped, then made writable. */
	for (uint32_t page = 0U; page < FAULT_PAGES; page++) {
		(void)__atomic_fetch_add(&g_fault_read_arrive[page], 1U, __ATOMIC_ACQ_REL);

		uint32_t spins = 0U;

		while (__atomic_load_n(&g_fault_read_arrive[page], __ATOMIC_ACQUIRE) < FAULT_THREADS) {
			if ((++spins & 0xFFFU) == 0U && now_us() > deadline) {
				(void)__atomic_fetch_add(&g_fault_bad, 1U, __ATOMIC_ACQ_REL);
				return;
			}
		}

		if (g_fault_read_base[(uint64_t)page * 512ULL + 100ULL] != 0ULL) (void)__atomic_fetch_add(&g_fault_bad, 1U, __ATOMIC_ACQ_REL);

		g_fault_read_base[(uint64_t)page * 512ULL + 200ULL + index] = page + 1U;
	}
}

static void
fault_thread(void *argument)
{
	fault_run((uint32_t)(uint64_t)argument);
	(void)__atomic_fetch_add(&g_fault_done, 1U, __ATOMIC_ACQ_REL);
	(void)nxu_thread_exit(0ULL);
}

static int
case_concurrent_faults(void)
{
	int64_t base = nxu_mmap(FAULT_PAGES * PAGE, NXU_MMAP_PROT_READ_WRITE);
	int64_t read_base = nxu_mmap(FAULT_PAGES * PAGE, NXU_MMAP_PROT_READ_WRITE);

	if (base <= 0 || read_base <= 0) return fail(4, "mmap");

	g_fault_base = (volatile uint64_t *)base;
	g_fault_read_base = (volatile uint64_t *)read_base;
	g_fault_done = 0U;
	g_fault_bad = 0U;

	for (uint32_t page = 0U; page < FAULT_PAGES; page++) {
		g_fault_arrive[page] = 0U;
		g_fault_read_arrive[page] = 0U;
	}

	for (uint64_t index = 1ULL; index < FAULT_THREADS; index++) {
		if (nxu_thread_spawn(fault_thread, (void *)index, 16ULL * 1024ULL) <= 0) return fail(4, "thread_spawn");
	}

	fault_run(0U);

	if (!wait_at_least(&g_fault_done, FAULT_THREADS - 1U)) return fail(4, "threads did not finish");
	if (g_fault_bad != 0U) return fail(4, "a fault handler gave a wrong page or a thread stalled");

	for (uint32_t page = 0U; page < FAULT_PAGES; page++) {
		for (uint32_t index = 0U; index < FAULT_THREADS; index++) {
			if (g_fault_base[(uint64_t)page * 512ULL + index] != 0x1000ULL * (index + 1U) + page) return fail(4, "a write to a page faulted in concurrently was lost");
			if (g_fault_read_base[(uint64_t)page * 512ULL + 200ULL + index] != page + 1U) return fail(4, "a write after a concurrent read fault was lost");
		}

		if (g_fault_base[(uint64_t)page * 512ULL + 100ULL] != 0ULL) return fail(4, "a page was not zero-filled");
	}

	say("smptest: 4 concurrent page faults: ");
	say_number(2ULL * FAULT_PAGES);
	say(" pages each faulted by ");
	say_number(FAULT_THREADS);
	say(" threads at once, all correct\n");

	(void)nxu_munmap((uint64_t)base, FAULT_PAGES * PAGE);
	(void)nxu_munmap((uint64_t)read_base, FAULT_PAGES * PAGE);
	return 0;
}

/* ---- case 5: shared memory between processes on different CPUs ----------- */

#define SHARED_INCREMENTS 20000U

static int
case_shared_memory(void)
{
	shared_reset();

	int64_t children[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		int64_t pid = nxu_fork();

		if (pid < 0) return fail(5, "fork");

		if (pid == 0) {
			(void)nxu_setaffinity(1ULL << nth_online(index + 1U));

			for (uint32_t count = 0U; count < SHARED_INCREMENTS; count++) {
				(void)__atomic_fetch_add(&g_shared->counter, 1ULL, __ATOMIC_ACQ_REL);
			}

			/* Publish a block, then the flag: the reader must see all of it. */
			if (index == 0U) {
				for (uint32_t word = 0U; word < 64U; word++) g_shared->block[word] = 0xB10C0000ULL + word;
				__atomic_store_n(&g_shared->block_ready, 1U, __ATOMIC_RELEASE);
			}

			nxu_exit(0ULL);
		}

		children[index] = pid;
	}

	(void)nxu_setaffinity(1ULL << nth_online(0U));

	for (uint32_t count = 0U; count < SHARED_INCREMENTS; count++) {
		(void)__atomic_fetch_add(&g_shared->counter, 1ULL, __ATOMIC_ACQ_REL);
	}

	bool ok = wait_at_least(&g_shared->block_ready, 1U);

	for (uint32_t word = 0U; ok && word < 64U; word++) {
		if (g_shared->block[word] != 0xB10C0000ULL + word) ok = false;
	}

	for (uint32_t index = 0U; index < 3U; index++) {
		uint64_t status = 0ULL;

		if (nxu_wait((uint64_t)children[index], &status) != children[index] || status != 0ULL) ok = false;
	}

	(void)nxu_setaffinity(0xFFULL);

	say("smptest: 5 shared memory: counter ");
	say_number(g_shared->counter);
	say(" of ");
	say_number(4ULL * SHARED_INCREMENTS);
	say("\n");

	if (!ok) return fail(5, "a block published across CPUs was not seen whole, or a process failed");
	if (g_shared->counter != 4ULL * SHARED_INCREMENTS) return fail(5, "atomic increments from different CPUs were lost");
	return 0;
}

/* ---- case 6: IPC between processes on different CPUs -------------------- */

#define IPC_ROUNDS 400U

static int
case_ipc(void)
{
	int64_t request_port = nxu_ipc_port_allocate();
	int64_t reply_port = nxu_ipc_port_allocate();

	if (request_port <= 0 || reply_port <= 0) return fail(6, "port allocation");

	uint32_t parent_cpu = nth_online(0U);
	uint32_t child_cpu = nth_online(g_online_count - 1U);

	int64_t pid = nxu_fork();

	if (pid < 0) return fail(6, "fork");

	if (pid == 0) {
		(void)nxu_setaffinity(1ULL << child_cpu);

		for (uint32_t round = 0U; round < IPC_ROUNDS; round++) {
			uint64_t message[2];

			if (nxu_ipc_receive_wait((uint32_t)request_port, message, sizeof(message), 0, 0) != (int64_t)sizeof(message)) nxu_exit(1ULL);

			message[0] += 1ULL;
			message[1] = (uint64_t)nxu_getcpu();

			if (nxu_ipc_send((uint32_t)reply_port, message, sizeof(message), 0U) != 0) nxu_exit(2ULL);
		}

		nxu_exit(0ULL);
	}

	(void)nxu_setaffinity(1ULL << parent_cpu);

	bool ok = true;
	uint32_t crossed = 0U;
	uint64_t started = now_us();

	for (uint32_t round = 0U; round < IPC_ROUNDS && ok; round++) {
		uint64_t message[2] = { 1000ULL + round, 0ULL };

		if (nxu_ipc_send((uint32_t)request_port, message, sizeof(message), 0U) != 0) {
			ok = false;
			break;
		}

		uint64_t reply[2];

		if (nxu_ipc_receive_wait((uint32_t)reply_port, reply, sizeof(reply), 0, 0) != (int64_t)sizeof(reply)) {
			ok = false;
			break;
		}

		if (reply[0] != 1001ULL + round) ok = false;
		if (reply[1] != (uint64_t)nxu_getcpu()) crossed++;
	}

	uint64_t elapsed = now_us() - started;
	uint64_t status = 0ULL;

	if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) ok = false;

	(void)nxu_setaffinity(0xFFULL);

	say("smptest: 6 IPC: ");
	say_number(IPC_ROUNDS);
	say(" blocking round trips between CPU ");
	say_number(parent_cpu);
	say(" and CPU ");
	say_number(child_cpu);
	say(" in ");
	say_number(elapsed);
	say(" us, ");
	say_number(crossed);
	say(" replies came from another CPU\n");

	if (!ok) return fail(6, "an IPC message was lost or corrupted");
	if (g_online_count >= 2U && crossed != IPC_ROUNDS) return fail(6, "sender and receiver were not on different CPUs");
	return 0;
}

/* ---- case 7: killing and exiting a process whose threads run on other CPUs -- */

static void
spinner_thread(void *argument)
{
	(void)argument;

	(void)__atomic_fetch_add(&g_shared->ready, 1U, __ATOMIC_ACQ_REL);

	uint32_t spins = 0U;

	while (!__atomic_load_n(&g_shared->stop, __ATOMIC_ACQUIRE)) {
		if ((++spins & 0x3FFU) == 0U) note_cpu(&g_shared->spin_mask);
	}

	(void)nxu_thread_exit(0ULL);
}

static int
case_termination(void)
{
	uint32_t killed = 0U;
	uint32_t exited = 0U;

	for (uint32_t variant = 0U; variant < 2U; variant++) {
		for (uint32_t iteration = 0U; iteration < 5U; iteration++) {
			shared_reset();

			int64_t pid = nxu_fork();

			if (pid < 0) return fail(7, "fork");

			if (pid == 0) {
				for (uint64_t index = 0ULL; index < 3ULL; index++) {
					if (nxu_thread_spawn(spinner_thread, (void *)index, 16ULL * 1024ULL) <= 0) nxu_exit(90ULL);
				}

				(void)__atomic_fetch_add(&g_shared->ready, 1U, __ATOMIC_ACQ_REL);

				/* Wait for the spinners, then either exit with them still running or wait to be killed. */
				if (!wait_at_least(&g_shared->ready, 4U)) nxu_exit(91ULL);

				if (variant == 1U) nxu_exit(7ULL);

				for (;;) note_cpu(&g_shared->spin_mask);
			}

			if (!wait_at_least(&g_shared->ready, 4U)) return fail(7, "the child's threads did not all start");

			/* Let the spinners settle onto CPUs. */
			uint64_t settle = now_us() + 20000ULL;

			while (now_us() < settle) __asm__ volatile("yield");

			if (variant == 0U && nxu_kill((uint64_t)pid, NXU_SIGKILL) != 0) return fail(7, "kill");

			uint64_t status = 0ULL;
			int64_t reaped = nxu_wait((uint64_t)pid, &status);

			if (reaped != pid) return fail(7, "wait did not return the child");

			if (variant == 0U) {
				if (!NXU_EXIT_WAS_KILLED(status)) return fail(7, "a killed process did not report being killed");
				killed++;
			} else {
				if (status != 7ULL) return fail(7, "a process that exited with threads running elsewhere returned the wrong status");
				exited++;
			}

			/* Whatever was left of it is gone: no child remains to be waited for. */
			uint64_t leftover = 0ULL;

			if (nxu_wait(NXU_WAIT_ANY, &leftover) != -NXU_SYS_E_NOT_FOUND) return fail(7, "a dead process left a child behind");
		}
	}

	say("smptest: 7 termination: ");
	say_number(killed);
	say(" processes killed and ");
	say_number(exited);
	say(" exited while their other threads spun on other CPUs\n");
	return 0;
}

/* ---- case 8: a thread's state survives being moved between CPUs --------- */

#define MIGRATE_ITERATIONS 300000U
#define MIGRATE_HOP 3000U

static uint64_t
migrate_work(bool hop, uint32_t member, uint32_t *mask, uint32_t *hops)
{
	uint64_t x = 88172645463325252ULL + member;
	uint64_t accumulator = 0ULL;

	for (uint32_t index = 0U; index < MIGRATE_ITERATIONS; index++) {
		x ^= x << 13U;
		x ^= x >> 7U;
		x ^= x << 17U;
		accumulator += x * 2654435761ULL + index;

		if (hop && (index % MIGRATE_HOP) == MIGRATE_HOP - 1U) {
			uint32_t target = nth_online(index / MIGRATE_HOP + member);

			if (nxu_setaffinity(1ULL << target) != 0) return 0ULL;
			if ((uint32_t)nxu_getcpu() != target) return 0ULL;

			*mask |= 1U << target;
			(*hops)++;
		}
	}

	return accumulator ^ x;
}

static volatile uint32_t g_migrate_done;
static volatile uint32_t g_migrate_bad;
static volatile uint32_t g_migrate_mask;
static volatile uint32_t g_migrate_hops;

static void
migrate_thread(void *argument)
{
	uint32_t member = (uint32_t)(uint64_t)argument;
	uint32_t mask = 0U;
	uint32_t hops = 0U;

	uint64_t reference = migrate_work(false, member, &mask, &hops);
	uint64_t moved = migrate_work(true, member, &mask, &hops);

	if (moved != reference || hops == 0U) (void)__atomic_fetch_add(&g_migrate_bad, 1U, __ATOMIC_ACQ_REL);

	(void)__atomic_fetch_or(&g_migrate_mask, mask, __ATOMIC_ACQ_REL);
	(void)__atomic_fetch_add(&g_migrate_hops, hops, __ATOMIC_ACQ_REL);
	(void)__atomic_fetch_add(&g_migrate_done, 1U, __ATOMIC_ACQ_REL);
	(void)nxu_thread_exit(0ULL);
}

static int
case_migration(void)
{
	g_migrate_done = 0U;
	g_migrate_bad = 0U;
	g_migrate_mask = 0U;
	g_migrate_hops = 0U;

	for (uint64_t member = 1ULL; member < 4ULL; member++) {
		if (nxu_thread_spawn(migrate_thread, (void *)member, 16ULL * 1024ULL) <= 0) return fail(8, "thread_spawn");
	}

	uint32_t mask = 0U;
	uint32_t hops = 0U;
	uint64_t reference = migrate_work(false, 0U, &mask, &hops);
	uint64_t moved = migrate_work(true, 0U, &mask, &hops);

	bool ok = moved == reference && hops != 0U;

	if (!wait_at_least(&g_migrate_done, 3U)) return fail(8, "threads did not finish");

	(void)nxu_setaffinity(0xFFULL);

	say("smptest: 8 migration: ");
	say_number(hops + g_migrate_hops);
	say(" forced moves across ");
	say_number(popcount(mask | g_migrate_mask));
	say(" CPUs, computation identical to the unmoved run\n");

	if (!ok || g_migrate_bad != 0U) return fail(8, "a computation changed result after its thread moved between CPUs");
	if (popcount(mask | g_migrate_mask) < 2U) return fail(8, "threads did not visit different CPUs");
	return 0;
}

/* ---- case 9: mmap, touch, munmap, fork and shared memory in one address space at once */

#define VM_STRESS_THREADS 4U
#define VM_STRESS_ITERATIONS 80U

static volatile uint32_t g_vm_done;
static volatile uint32_t g_vm_bad;

static void
vm_stress_run(uint32_t member)
{
	uint64_t seed = 0x9E3779B97F4A7C15ULL * (member + 1U);

	for (uint32_t iteration = 0U; iteration < VM_STRESS_ITERATIONS; iteration++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;

		if (member == 3U && (iteration % 8U) == 0U) {
			/* One thread also forks now and then while the others fault and unmap. */
			int64_t pid = nxu_fork();

			if (pid == 0) nxu_exit(0ULL);

			uint64_t status = 0ULL;

			if (pid < 0 || nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) (void)__atomic_fetch_add(&g_vm_bad, 1U, __ATOMIC_ACQ_REL);

			continue;
		}

		uint64_t pages = 1ULL + (seed >> 33U) % 8ULL;
		int64_t base = nxu_mmap(pages * PAGE, NXU_MMAP_PROT_READ_WRITE);

		if (base <= 0) {
			(void)__atomic_fetch_add(&g_vm_bad, 1U, __ATOMIC_ACQ_REL);
			continue;
		}

		volatile uint64_t *words = (volatile uint64_t *)base;
		uint64_t stamp = ((uint64_t)member << 48U) | ((uint64_t)iteration << 32U);

		for (uint64_t page = 0ULL; page < pages; page++) words[page * 512ULL + member] = stamp + page;

		(void)nxu_yield();

		for (uint64_t page = 0ULL; page < pages; page++) {
			if (words[page * 512ULL + member] != stamp + page) (void)__atomic_fetch_add(&g_vm_bad, 1U, __ATOMIC_ACQ_REL);
			if (words[page * 512ULL + 511ULL] != 0ULL) (void)__atomic_fetch_add(&g_vm_bad, 1U, __ATOMIC_ACQ_REL);
		}

		if (nxu_munmap((uint64_t)base, pages * PAGE) != 0) (void)__atomic_fetch_add(&g_vm_bad, 1U, __ATOMIC_ACQ_REL);

	}
}

static void
vm_stress_thread(void *argument)
{
	vm_stress_run((uint32_t)(uint64_t)argument);
	(void)__atomic_fetch_add(&g_vm_done, 1U, __ATOMIC_ACQ_REL);
	(void)nxu_thread_exit(0ULL);
}

static int
case_vm_stress(void)
{
	g_vm_done = 0U;
	g_vm_bad = 0U;

	for (uint64_t member = 1ULL; member < VM_STRESS_THREADS; member++) {
		if (nxu_thread_spawn(vm_stress_thread, (void *)member, 16ULL * 1024ULL) <= 0) return fail(9, "thread_spawn");
	}

	vm_stress_run(0U);

	if (!wait_at_least(&g_vm_done, VM_STRESS_THREADS - 1U)) return fail(9, "threads did not finish");

	say("smptest: 9 VM stress: ");
	say_number(VM_STRESS_THREADS * VM_STRESS_ITERATIONS);
	say(" map/touch/unmap/fork rounds across ");
	say_number(VM_STRESS_THREADS);
	say(" threads, ");
	say_number(g_vm_bad);
	say(" errors\n");

	return g_vm_bad == 0U ? 0 : fail(9, "the address space gave a wrong answer under concurrent map, fault, unmap and fork");
}

/* ---- case 10: shm registry churn ------------------------------------------ */

/*
 * More than the registry's fixed slot count (SHM_REGISTRY_MAX, 64 --
 * kern/ipc/shm_registry.c) so a withdraw that failed to free its slot runs
 * the registry out of room well before this loop ends: shm_create would
 * start failing with NO_SPACE partway through instead of at the very end.
 */
#define SHM_CHURN_ITERATIONS 96U
#define SHM_CHURN_BYTES 4096ULL

static int
case_shm_churn(void)
{
	for (uint32_t iteration = 0U; iteration < SHM_CHURN_ITERATIONS; iteration++) {
		int64_t id = nxu_shm_create(SHM_CHURN_BYTES);
		if (id <= 0) return fail(10, "shm_create");

		int64_t va = nxu_shm_map((uint64_t)id);
		if (va <= 0) return fail(10, "shm_map");

		*(volatile uint32_t *)va = iteration;
		if (*(volatile uint32_t *)va != iteration) return fail(10, "wrote through a mapping and read something else back");

		if (nxu_shm_unmap((uint64_t)va) != 0) return fail(10, "shm_unmap");
		if (nxu_shm_withdraw((uint64_t)id) != 0) return fail(10, "shm_withdraw");
	}

	say("smptest: 10 shm churn: ");
	say_number(SHM_CHURN_ITERATIONS);
	say(" publish/attach/detach/withdraw cycles, all succeeded\n");

	return 0;
}

/* ---- case 11: withdraw does not disturb an existing attachment ----------- */

static int
case_shm_withdraw_ordering(void)
{
	int64_t id = nxu_shm_create(SHM_CHURN_BYTES);
	if (id <= 0) return fail(11, "shm_create");

	int64_t va = nxu_shm_map((uint64_t)id);
	if (va <= 0) return fail(11, "shm_map");

	*(volatile uint32_t *)va = 0xC0FFEEU;

	/*
	 * Withdrawing while still mapped stops the id from resolving to a new
	 * attach, but must not touch this process's own reference or mapping --
	 * shm_registry_withdraw only ever releases the reference publish itself
	 * took out.
	 */
	if (nxu_shm_withdraw((uint64_t)id) != 0) return fail(11, "shm_withdraw");
	if (nxu_shm_map((uint64_t)id) != -NXU_SYS_E_NOT_FOUND) return fail(11, "a withdrawn id still resolved to shm_map");
	if (*(volatile uint32_t *)va != 0xC0FFEEU) return fail(11, "withdraw corrupted an attachment made before it");

	if (nxu_shm_unmap((uint64_t)va) != 0) return fail(11, "shm_unmap");

	/* The registry slot was freed by the withdraw above, not by this unmap:
	 * a fresh create must be able to reuse it right away. */
	int64_t reused = nxu_shm_create(SHM_CHURN_BYTES);
	if (reused <= 0) return fail(11, "shm_create did not reuse the withdrawn slot");
	if (nxu_shm_withdraw((uint64_t)reused) != 0) return fail(11, "shm_withdraw (cleanup)");

	say("smptest: 11 shm withdraw ordering: withdraw-before-unmap kept the mapping alive and freed the id\n");
	return 0;
}

/* ---- main ---------------------------------------------------------------- */

int
main(void)
{
	g_online = probe_online_cpus();
	g_online_count = popcount(g_online);

	say("smptest: ");
	say_number(g_online_count);
	say(" CPUs online (mask ");
	say_number(g_online);
	say("), running on CPU ");
	say_number((uint64_t)nxu_getcpu());
	say("\n");

	if (g_online_count < 2U) return fail(100, "needs at least two CPUs");
	if (!shared_create()) return fail(101, "shared memory setup");

	int result;

	if ((result = case_thread_ring()) != 0) return result;
	if ((result = case_process_ring()) != 0) return result;
	if ((result = case_isolation()) != 0) return result;
	if ((result = case_concurrent_faults()) != 0) return result;
	if ((result = case_shared_memory()) != 0) return result;
	if ((result = case_ipc()) != 0) return result;
	if ((result = case_termination()) != 0) return result;
	if ((result = case_migration()) != 0) return result;
	if ((result = case_vm_stress()) != 0) return result;
	if ((result = case_shm_churn()) != 0) return result;
	if ((result = case_shm_withdraw_ordering()) != 0) return result;

	say("smptest: passed\n");
	return 0;
}
