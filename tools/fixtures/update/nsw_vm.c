/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <nxu/syscall.h>

int
main(void)
{
	bool success = nsw_prepare("/Volumes/Recovery/mainOS/build-vm-test.nsw", "/Volumes/Recovery/Prepared");
	const char *message = success ? "nsw-vm: PASS\n" : "nsw-vm: FAIL\n";
	(void)nxu_write(1, message, 13);
	return success ? 0 : 1;
}
