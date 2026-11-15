/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_SOFTWARE_UPDATE_NSW_H
#define NXU_SOFTWARE_UPDATE_NSW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	char build[64];
	char architecture[16];
	char system[128];
	char system_hash[65];
	char recovery[128];
	char recovery_hash[65];
} nsw_manifest_t;

#define NSW_RECOVERY_ROOT "/Volumes/Recovery"
#define NSW_PREPARED_ROOT NSW_RECOVERY_ROOT "/Prepared"
#define NSW_TRANSACTIONS_ROOT NSW_RECOVERY_ROOT "/Transactions"
#define NSW_ACTIVE_UPDATE "/Volumes/mainOS/var/db/patchd/update-active"
#define NSW_INSTALL_PENDING NSW_RECOVERY_ROOT "/InstallPending"
#define NSW_INSTALL_READY NSW_RECOVERY_ROOT "/InstallReady"

/*
 * Erase All Content and Settings. patchd queues an erase on the recovery
 * volume; at the next boot the kernel starts the recovery volume's patchd as
 * PID 1 with NSW_ERASE_ARGUMENT, which wipes /Volumes/mainOS and reinstalls
 * it from the verified factory payload. See doc/erase-and-startup-disk.md.
 */
#define NSW_ERASE_ROOT NSW_RECOVERY_ROOT "/Erase"
#define NSW_ERASE_QUEUED NSW_ERASE_ROOT "/Queued"
#define NSW_ERASE_FAILED NSW_ERASE_ROOT "/Failed"
#define NSW_ERASE_LOG NSW_ERASE_ROOT "/Log.txt"
#define NSW_ERASE_MARKER "erase-all-content-and-settings\n"
#define NSW_ERASE_PROGRAM NSW_RECOVERY_ROOT "/System/Recovery/patchd"
#define NSW_ERASE_ARGUMENT "--erase-all-content-and-settings"
#define NSW_FACTORY_ROOT NSW_RECOVERY_ROOT "/System/Recovery/Factory"
#define NSW_FACTORY_FILES NSW_FACTORY_ROOT "/Root"
#define NSW_FACTORY_MANIFEST NSW_FACTORY_ROOT "/FactoryManifest"
#define NSW_FACTORY_MANIFEST_HASH NSW_FACTORY_ROOT "/FactoryManifest.sha256"

/*
 * Startup Disk. The choice lives on the recovery volume (NVRAM cannot be
 * written under QEMU); patchd mirrors it on the system volume for System
 * Settings. Recovery is honoured once, then the kernel resets it to mainOS.
 */
#define NSW_STARTUP_DISK NSW_RECOVERY_ROOT "/System/StartupDisk"
#define NSW_STARTUP_DISK_MIRROR "/Volumes/mainOS/var/db/patchd/startup-disk"

typedef struct {
	char build[64];
	char transaction[96];
	uint8_t manifest_hash[32];
} nsw_request_t;

bool nsw_component(const char *name);
bool nsw_manifest_parse(const char *data, uint32_t length, nsw_manifest_t *manifest);
bool nsw_request_parse(const char *data, uint32_t length, nsw_request_t *request);
bool nsw_hash_decode(const char *text, uint8_t digest[32]);

/* Verify and stage an ISO/Joliet NSW package; never modifies the live system. */
bool nsw_prepare(const char *package, const char *prepared_root);

/* Local administrator selection; queued installs run before userland at reboot. */
bool nsw_schedule(const char *build);
bool nsw_confirm_boot(void);

#endif
