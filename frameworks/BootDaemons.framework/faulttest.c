#include <nxu/syscall.h>
#include <nxu/thread.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway user-fault test: spawned directly by
 * kern/tests/fault_process_test.c, once per mode. Each mode commits a
 * different process-level crime (bad pointer, write to text, jump into
 * data, undefined instruction, breakpoint, kernel address, a fault on a
 * secondary thread) and the kernel is expected to kill just this process
 * with the matching signal instead of panicking. Mode 7 is the control: it
 * exits normally.
 *
 * The mode arrives through /fault-mode on the root ramfs, a single ASCII
 * digit the kernel test writes before each spawn (spawn carries no argv).
 */

#define FAULTTEST_MODE_PATH "/fault-mode"
#define FAULTTEST_STACK_SIZE (16ULL * 1024ULL)
#define FAULTTEST_WAIT_ITERATIONS 2000000U
#define FAULTTEST_CONTROL_STATUS 42ULL

/*
 * Explicit section: left alone, clang notices nothing ever stores to this
 * array and files it under .rodata, which user.ld puts in the executable
 * text segment -- and then the "jump into data" mode legitimately runs.
 */
static volatile uint32_t g_data_words[4] __attribute__((section(".data.faulttest"))) = {
	0xD503201FU, 0xD503201FU, 0xD65F03C0U, 0U
};

static int64_t
faulttest_mode(void)
{
	int64_t descriptor = nxu_open(FAULTTEST_MODE_PATH, NXU_O_READ);
	if (descriptor < 0) return -1;

	char digit = 0;
	int64_t count = nxu_read((uint64_t)descriptor, &digit, 1ULL);
	(void)nxu_close((uint64_t)descriptor);

	if (count != 1 || digit < '0' || digit > '9') return -1;
	return (int64_t)(digit - '0');
}

static void
faulttest_thread_fault(void *arg)
{
	(void)arg;
	*(volatile uint64_t *)0 = 1ULL;
	(void)nxu_thread_exit(0ULL);
}

int
main(void)
{
	switch (faulttest_mode()) {
	case 0:
		/* Null read: inside the unmapped guard page. */
		return (int)*(volatile uint32_t *)0;

	case 1:
		/* Write to our own text: mapped read-execute, so a permission fault. */
		*(volatile uint32_t *)(uintptr_t)main = 0U;
		break;

	case 2:
		/* Execute from data: mapped read-write but never executable. */
		((void (*)(void))(uintptr_t)&g_data_words[0])();
		break;

	case 3:
		/* Permanently undefined instruction. */
		__asm__ volatile(".inst 0x00000000");
		break;

	case 4:
		__asm__ volatile("brk #0");
		break;

	case 5:
		/* A kernel address from EL0. */
		return (int)*(volatile uint32_t *)0xFFFF000000001000ULL;

	case 6: {
		/* A secondary thread faults; the whole process must die. */
		int64_t tid = nxu_thread_spawn(faulttest_thread_fault, 0, FAULTTEST_STACK_SIZE);
		if (tid <= 0) return 1;

		for (uint32_t spin = 0U; spin < FAULTTEST_WAIT_ITERATIONS; spin++) (void)nxu_yield();
		return 2;
	}

	case 7:
		return (int)FAULTTEST_CONTROL_STATUS;

	default:
		return 3;
	}

	/* Every faulting mode above must have been killed before getting here. */
	return 4;
}
