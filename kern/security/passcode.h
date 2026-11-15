/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/passcode.h
 *
 * The login passcode: what unlocks the desktop (drivers/video/
 * keystore_manager.c, drivers/video/ui_service_login.c), what the setup
 * assistant sets on first boot, and what the privacy prompt
 * (kern/security/sap.c) asks for. It is kept by the kernel on the system
 * volume and needs nothing else: no enclave, no second machine.
 *
 * The record is /var/db/passcode on the system volume (the kernel sees it
 * as /Volumes/mainOS/var/db/passcode), owned by root, mode 0600. It holds
 * a PBKDF2-HMAC-SHA256 hash in the /etc/shadow format, never the passcode,
 * and the failure count:
 *
 *   - The count is written to disk before a passcode is compared, so
 *     turning the machine off mid-check does not undo a failure.
 *   - From the fifth failure in a row each attempt waits (1 min, 5 min,
 *     15 min, 1 h, 1 h); the tenth locks the passcode. A right passcode
 *     clears the count.
 *   - Locked, or forgotten: remove the record from Recovery (or Erase All
 *     Content, which removes it with everything else) and the next boot
 *     asks for a new passcode, as on first boot.
 *
 * Like any passcode checked by the machine it protects, this stops someone
 * at the keyboard, not someone holding the disk.
 */

#ifndef NXU_KERN_SECURITY_PASSCODE_H
#define NXU_KERN_SECURITY_PASSCODE_H

#include <stdbool.h>
#include <stdint.h>

#define PASSCODE_MIN_LENGTH 4U
#define PASSCODE_MAX_LENGTH 64U

/* Where the record lives, as the kernel sees it and as processes do. */
#define PASSCODE_RECORD_PATH "/Volumes/mainOS/var/db/passcode"
#define PASSCODE_RECORD_USER_PATH "/var/db/passcode"

typedef enum {
	PASSCODE_OK = 0,
	PASSCODE_ERR_UNAVAILABLE, /* the system volume is not mounted, or the record could not be written */
	PASSCODE_ERR_INVALID,	  /* lengths out of range, or an old passcode given when none is set */
	PASSCODE_ERR_NOT_FOUND,	  /* no passcode is set */
	PASSCODE_ERR_DENIED,	  /* wrong passcode */
	PASSCODE_ERR_RETRY_LATER, /* too soon after failed passcodes; *wait_seconds says how long */
	PASSCODE_ERR_LOCKED		  /* ten failures in a row, or a damaged record */
} passcode_result_t;

typedef struct {
	bool passcode_set;
	bool locked;
	uint8_t failures;	   /* failed attempts since the last success */
	uint32_t wait_seconds; /* before another attempt is taken */
} passcode_status_t;

passcode_result_t passcode_status(passcode_status_t *status);

/* PASSCODE_ERR_RETRY_LATER sets *wait_seconds; every other result sets it to 0. */
passcode_result_t passcode_verify(const void *passcode, uint32_t length, uint32_t *wait_seconds);

/*
 * Set the passcode. With none set yet, old_length is 0. With one set, the
 * old passcode must be given and is checked like passcode_verify (it counts
 * as an attempt and waits like one).
 */
passcode_result_t passcode_set(const void *old_passcode, uint32_t old_length, const void *passcode, uint32_t length,
							   uint32_t *wait_seconds);

const char *passcode_result_name(passcode_result_t result);

#endif
