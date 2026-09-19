/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_file.h
 *
 * File data. A regular file (and a symlink, whose target is its data) is a
 * sorted run of EXTENT_DATA items (ino, 108, file offset), each describing
 * what the file holds from that offset:
 *
 *   inline      the bytes themselves, stored in the item (small files,
 *               symlink targets); always at file offset 0
 *   regular     `num_bytes` of the file are bytes [offset, offset+num_bytes)
 *               of a disk extent of `disk_num_bytes` at logical address
 *               `disk_bytenr`. Several items may reference slices of one disk
 *               extent (after partial overwrites); `offset` says where the
 *               slice starts.
 *   prealloc    reserved, unwritten space: reads as zeros
 *   hole        a regular extent with disk_bytenr 0 (explicit hole) or simply
 *               a gap between two items (NO_HOLES); reads as zeros
 *
 * Bytes past the last extent up to the inode size read as zeros. Reads are
 * clipped at the inode size.
 *
 * Compressed extents (compression != 0) are decoded through the decompressor
 * table in btrfs_fs_t (fs->decompressors[]); with no decoder registered the
 * read fails with BTRFS_ERR_UNSUPPORTED_COMPRESSION, never with garbage.
 * Encrypted extents fail with BTRFS_ERR_UNSUPPORTED_ENCRYPTION.
 *
 * With options.verify_data_csums the sectors of every regular extent read are
 * checked against the csum tree (EXTENT_CSUM items), falling back to the other
 * copy of a DUP chunk, and a mismatch returns BTRFS_ERR_CSUM.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_FILE_H
#define NXU_VFS_BTRFS_BTRFS_FILE_H

#include "btrfs_fs.h"
#include "btrfs_inode.h"

/*
 * Read up to length bytes at offset. *done is the number of bytes produced
 * (less than length only at the end of the file). The inode must be a regular
 * file or a symlink.
 */
btrfs_status_t btrfs_file_read(btrfs_fs_t *fs, const btrfs_tree_t *subvol, const btrfs_inode_t *inode, uint64_t offset, void *buffer, uint64_t length, uint64_t *done);

/* The target of a symlink inode: at most capacity bytes, not NUL terminated. */
btrfs_status_t btrfs_file_readlink(btrfs_fs_t *fs, const btrfs_tree_t *subvol, const btrfs_inode_t *inode, char *buffer, size_t capacity, size_t *length);

#endif
