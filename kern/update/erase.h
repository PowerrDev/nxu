/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/update/erase.h
 *
 * Declares the boot hook for "Erase All Content and Settings": the kernel
 * finds an erase queued on the recovery volume and starts patchd from that
 * volume, as PID 1, to wipe and reinstall the system volume.
 */
#ifndef NXU_KERN_UPDATE_ERASE_H
#define NXU_KERN_UPDATE_ERASE_H

#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>

#include <stdbool.h>

/*
 * Run once the system and recovery volumes are mounted and any software
 * update has been resolved. Decides whether this boot erases the system
 * volume, or (after an erase that did not finish) must start recovery.
 */
void boot_erase_init(void);

/* An erase is queued and this boot is not a recovery boot: start the eraser instead of bootd. */
bool boot_erase_pending(void);

/* Start patchd from the recovery volume in erase mode as the first process. */
loader_status_t boot_erase_spawn(proc_t *result);

/*
 * The eraser is running as PID 1. The system call layer lets that one
 * process remove protected system files, which it must do to wipe them.
 */
bool boot_erase_running(void);

#endif
