/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * suidhelper -- the target of the setuid-exec demonstration (Phase 4,
 * doc/architecture/unix-security.md). It does nothing but report, through its
 * exit code, the effective UID it was given: 7 when it runs as root (euid 0),
 * 8 otherwise. suidtest sets this binary setuid-root, drops to a non-root UID,
 * spawns it, and reads the code back. Spawned without stdio, so it speaks only
 * through its exit status.
 */
#include <nxu/syscall.h>

#include <stdint.h>

int
main(void)
{
	return nxu_geteuid() == 0 ? 7 : 8;
}
