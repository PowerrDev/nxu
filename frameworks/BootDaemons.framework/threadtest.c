#include <nxu/syscall.h>
#include <nxu/thread.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway user-thread test: spawned directly by
 * kern/tests/thread_process_test.c. Every worker thread shares this
 * process's address space (static storage, not IPC or shared memory) with
 * the main thread -- the point of the test is proving nxu_thread_spawn
 * (nxu_mmap for a private stack + nxu_thread_create) actually runs
 * concurrently-scheduled threads that see the same globals, with the
 * caller-supplied argument arriving correctly in each one.
 */

#define THREADTEST_THREAD_COUNT 3U
#define THREADTEST_STACK_SIZE (16ULL * 1024ULL)
#define THREADTEST_WAIT_ITERATIONS 2000000U
#define THREADTEST_MARKER_BASE 0x5A5A0000ULL

static volatile uint64_t g_marker[THREADTEST_THREAD_COUNT];
static volatile uint64_t g_done_count;

static void
threadtest_worker(void *arg)
{
	uint64_t index = (uint64_t)arg;

	g_marker[index] = THREADTEST_MARKER_BASE + index;
	(void)__atomic_fetch_add(&g_done_count, 1ULL, __ATOMIC_SEQ_CST);

	(void)nxu_thread_exit(0ULL);
}

int
main(void)
{
	for (uint64_t index = 0ULL; index < THREADTEST_THREAD_COUNT; index++) {
		int64_t tid = nxu_thread_spawn(threadtest_worker, (void *)index, THREADTEST_STACK_SIZE);
		if (tid <= 0) return 1;
	}

	for (uint32_t spin = 0U; spin < THREADTEST_WAIT_ITERATIONS; spin++) {
		if (g_done_count == THREADTEST_THREAD_COUNT) break;
		(void)nxu_yield();
	}

	if (g_done_count != THREADTEST_THREAD_COUNT) return 2;

	for (uint64_t index = 0ULL; index < THREADTEST_THREAD_COUNT; index++) {
		if (g_marker[index] != THREADTEST_MARKER_BASE + index) return 3;
	}

	return 0;
}
