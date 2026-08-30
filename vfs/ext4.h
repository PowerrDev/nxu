#ifndef NXU_VFS_EXT4_H
#define NXU_VFS_EXT4_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ext4_register:
 *
 * Publish the writable ext4 filesystem implementation to VFS. Registration
 * creates no mount and performs no block I/O.
 */
bool ext4_register(void);

uint32_t ext4_mount_count(void);

/*
 * ext4_debug_arm_journal_crash:
 *
 * Arm the first active ext4 mount so the next metadata transaction stops
 * immediately after its durable JBD2 commit record. This hook exists only
 * for the deterministic journal-replay boot test.
 */
bool ext4_debug_arm_journal_crash(void);
bool ext4_debug_journal_crash_reached(void);

void ext4_dump(void);

#endif
