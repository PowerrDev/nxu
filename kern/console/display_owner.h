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
#include <frameworks/include/nxu/gui_host_protocol.h>

#include <stdbool.h>

/*
 * Gates NXU_SYS_RECOVERY_PRESENT/_DISPLAY_INFO for normal boot the same way
 * boot_mode_is_triage_os() gates them for recovery -- see
 * kern/syscall/syscall.c. Exactly one process (WindowServer) is expected to
 * hold this per boot. Ownership uses the full process identity, is
 * idempotent for that owner, and is revoked on exit or successful exec.
 */
bool display_owner_claim(proc_id_t pid);
bool display_owner_is(proc_id_t pid);
void display_owner_release(proc_t proc);
void display_owner_restart(void);
int64_t display_owner_host(proc_t proc, const nxu_gui_host_hello_t *request, nxu_gui_host_hello_reply_t *reply);

#endif
