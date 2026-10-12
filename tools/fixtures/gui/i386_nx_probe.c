/* A writable user mapping must fault on instruction fetch in ring 3. */

#include <nxu/syscall.h>

int
main(void)
{
	int64_t address = nxu_mmap(4096, NXU_MMAP_PROT_READ_WRITE);

	if (address < 0) {
		return 1;
	}

	*(volatile uint8_t *)(uintptr_t)address = 0xC3; /* RET */

	static const char ready[] = "nx-probe: attempting instruction fetch from writable user data\n";

	(void)nxu_write(1, ready, sizeof(ready) - 1);
	((void (*)(void))(uintptr_t)address)();

	static const char failed[] = "nx-probe: DATA EXECUTION SUCCEEDED\n";

	(void)nxu_write(1, failed, sizeof(failed) - 1);
	return 2;
}
