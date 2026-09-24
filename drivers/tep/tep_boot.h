/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_boot.h
 *
 * Measured boot, reported not enforced. Once tepOS offers its boot policy
 * service (TEP_MB_FEATURE_BOOT), a kernel thread hashes bootd as it is on
 * disk (SHA-256, libk/sha256.h), reads the manifest the disk build signed
 * for it (bootd.manifest, tepOS tools/boot_sign) and asks tepOS whether
 * the two agree. The verdict is logged and kept for tests.
 *
 * Nothing acts on the verdict: bootd starts either way, and the measurement
 * is taken by the same kernel that loads bootd, from the same disk. Without
 * a trusted boot chain that measures NXU itself, this detects a bootd that
 * does not match its signed manifest; it does not prevent one from running.
 */

#ifndef NXU_DRIVERS_TEP_TEP_BOOT_H
#define NXU_DRIVERS_TEP_TEP_BOOT_H

#include <drivers/tep/tep_crypto.h>

#include <stdbool.h>
#include <stdint.h>

#define TEP_BOOT_IMAGE_PATH "/disk/System/Library/CoreServices/bootd"
#define TEP_BOOT_MANIFEST_PATH "/disk/System/Library/CoreServices/bootd.manifest"
#define TEP_BOOT_IMAGE_NAME "bootd"

typedef enum {
	TEP_BOOT_CHECK_PENDING = 0,  /* not checked yet (tepOS or the disk not there yet) */
	TEP_BOOT_CHECK_VERIFIED,     /* tepOS accepted the manifest for the measured image */
	TEP_BOOT_CHECK_REFUSED,      /* tepOS refused it: denied or rollback */
	TEP_BOOT_CHECK_NO_MANIFEST,  /* the disk has no manifest for bootd */
	TEP_BOOT_CHECK_FAILED        /* could not measure, or tepOS could not answer */
} tep_boot_check_t;

/* Starts the measuring thread (once). False when it could not be started. */
bool tep_boot_check_start(void);

/*
 * The check's outcome so far. detail is the image version when verified,
 * the TEP_BOOT_* reason when denied, the minimum version on a rollback.
 */
tep_boot_check_t tep_boot_check_state(uint32_t *detail);

/* A manifest file: exactly TEP_BOOT_MANIFEST_SIZE bytes, or false. */
bool tep_boot_read_manifest(const char *path, uint8_t manifest[TEP_BOOT_MANIFEST_SIZE]);

/* SHA-256 of a whole file as it is on disk, and its size. */
bool tep_boot_measure(const char *path, uint8_t digest[TEP_SHA256_SIZE], uint64_t *size);

const char *tep_boot_check_name(tep_boot_check_t check);

#endif
