/* Exercise ELF32 binding, constructors, Runtime allocation and RELRO on the guest. */
#include <nxu/runtime.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static bool initialized;
static int64_t (*const process_id)(void) = nxu_getpid;

__attribute__((constructor)) static void initialize(void)
{
	initialized = true;
}

int main(void)
{
	uint8_t *memory = nxu_alloc(8193, 64);

	if (!initialized || process_id() <= 1 || !memory || ((uintptr_t)memory & 63))
		return 1;

	memset(memory, 0xa5, 8193);
	uint8_t *grown = nxu_realloc(memory, 16384, 64);

	if (!grown)
		return 2;

	for (uint32_t i = 0; i < 8193; i++)
		if (grown[i] != 0xa5)
			return 3;

	if (!nxu_free(grown))
		return 4;

	static const char ready[] = "framework-probe: allocator/binding/constructor PASS; writing RELRO\n";
	(void)nxu_write(1, ready, sizeof(ready) - 1);
	*(volatile uintptr_t *)(uintptr_t)&process_id = 0;

	static const char failed[] = "framework-probe: RELRO WRITE SUCCEEDED\n";
	(void)nxu_write(1, failed, sizeof(failed) - 1);
	return 5;
}
