#include <nxu/thread.h>

#include <nxu/syscall.h>

#include <stdint.h>

int64_t nxu_thread_spawn(void (*entry)(void *arg), void *arg, uint64_t stack_size)
{
	if (entry == 0 || stack_size == 0ULL) return -NXU_SYS_E_INVALID_ARGUMENT;

	int64_t base = nxu_mmap(stack_size, NXU_MMAP_PROT_READ_WRITE);
	if (base < 0) return base;

	void *stack_top = (void *)((uint64_t)base + stack_size);
	return nxu_thread_create(entry, stack_top, arg);
}
