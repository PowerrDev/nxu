/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/fs_test.h
 *
 * Filesystem write and journal tests for the mounted ext4 root volume,
 * selected with the boot argument fs-test=<mode>. They are meant to be run
 * by tools/test_i386_fs.sh, which checks the resulting disk image from the
 * host with e2fsck and debugfs, so the kernel is not just grading itself.
 *
 *   write           create, grow, truncate, sparse-extend, overwrite and unlink
 *                   files, verify them, leave a persistence marker (created on
 *                   the first run, verified on later ones) and sync.
 *   journal-crash   commit a metadata transaction to the journal, then lose
 *                   power before it is checkpointed to its home location.
 *   journal-verify  after a reboot the mount must have replayed that journal:
 *                   the file the crashed transaction created is present.
 */

#ifndef NXU_KERN_I386_FS_TEST_H
#define NXU_KERN_I386_FS_TEST_H

#include <stdbool.h>

/*
 * Run one mode against the filesystem mounted at /disk. Returns true on pass.
 * "journal-crash" does not return on success: it powers the machine off.
 */
bool i386_fs_test_run(const char *mode);

#endif
