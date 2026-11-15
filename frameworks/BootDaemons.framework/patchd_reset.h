/* Copyright (c) 2026 NXU Project. All rights reserved. */
/*
 * patchd's "Transfer or Reset" and "Startup Disk" work: the request files
 * System Settings writes, and the erase that runs at boot as PID 1.
 */
#ifndef NXU_PATCHD_RESET_H
#define NXU_PATCHD_RESET_H

#define PATCHD_ERASE_REQUEST "/Volumes/mainOS/var/db/patchd/erase-request"
#define PATCHD_ERASE_STATUS "/Volumes/mainOS/var/db/patchd/erase-status"
#define PATCHD_STARTUP_DISK_REQUEST "/Volumes/mainOS/var/db/patchd/startup-disk-request"

/* Service loop: consume a complete erase request, queue the erase and restart. */
void patchd_erase_requested(void);

/* Service loop: consume a complete startup disk request, store it, restart when asked. */
void patchd_startup_disk_requested(void);

/* At service start: make the system volume's mirror match the stored choice. */
void patchd_startup_disk_sync(void);

/* PID 1 erase mode, started by the kernel from the recovery volume. Does not return. */
int patchd_erase_run(void);

#endif
