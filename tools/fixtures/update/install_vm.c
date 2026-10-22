/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <nxu/string.h>
#include <nxu/syscall.h>

int
main(void)
{
	const char *marker = "/Volumes/mainOS/var/db/patchd/install-test-queued";
	int64_t descriptor = nxu_open(marker, NXU_O_READ);
	if (descriptor >= 0) {
		(void)nxu_close(descriptor);
		return 0;
	}
	descriptor = nxu_open(marker, NXU_O_WRITE | NXU_O_CREATE);
	if (descriptor < 0 || nxu_write(descriptor, "queued\n", 7) != 7 || nxu_close(descriptor) < 0 || nxu_sync() < 0) {
		const char *message = "install-vm: FAIL marker\n";
		(void)nxu_write(1, message, nxu_strlen(message));
		return 1;
	}

	/* Exercise the real daemon request, including patchd's reset capability. */
	descriptor = nxu_open("/Volumes/mainOS/var/db/patchd/install-update", NXU_O_WRITE | NXU_O_CREATE);
	if (descriptor < 0 || nxu_write(descriptor, "install-test\n", 13) != 13 || nxu_close(descriptor) < 0 ||
		nxu_sync() < 0) {
		const char *message = "install-vm: FAIL request\n";
		(void)nxu_write(1, message, nxu_strlen(message));
		return 1;
	}
	return 0;
}
