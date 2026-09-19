/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/console/display_owner.h
 *
 * First-come-first-served claim on presenting to the real display outside
 * triageOS/recovery boot.
 */

#ifndef NXU_KERN_CONSOLE_DISPLAY_OWNER_H
#define NXU_KERN_CONSOLE_DISPLAY_OWNER_H

#include <kern/process/proc.h>

#include <stdbool.h>

/*
 * Gates NXU_SYS_RECOVERY_PRESENT/_DISPLAY_INFO for normal boot the same way
 * boot_mode_is_triage_os() gates them for recovery -- see
 * kern/syscall/syscall.c. Exactly one process (WindowServer) is expected to
 * hold this per boot. Idempotent for the current owner; re-claiming after
 * the current owner has exited succeeds (proc_find returns 0 for a reaped
 * pid), otherwise fails while a live owner holds it.
 */
bool display_owner_claim(proc_id_t pid);
bool display_owner_is(proc_id_t pid);

#endif
