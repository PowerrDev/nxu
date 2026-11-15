/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/boot/startup_disk.h
 *
 * Declares the Startup Disk selection read at boot.
 */
#ifndef NXU_KERN_BOOT_STARTUP_DISK_H
#define NXU_KERN_BOOT_STARTUP_DISK_H

/*
 * Read the startup disk chosen in System Settings (stored on the recovery
 * volume by patchd). Run after the system and recovery volumes are mounted
 * and before PID 1 is chosen. A Recovery choice starts triageOS for this boot
 * only: it is reset to mainOS before recovery starts, so a restart from
 * recovery always comes back to mainOS.
 */
void boot_startup_disk_init(void);

#endif
