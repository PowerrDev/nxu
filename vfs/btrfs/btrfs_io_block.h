/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io_block.h
 *
 * Kernel-only: a btrfs_reader_t over a block_device_t. The block layer moves
 * whole 512-byte sectors; Btrfs reads arbitrary byte ranges (a tree block at
 * any sector-aligned address, file data at any offset), so this adapter reads
 * the aligned middle directly and bounces the partial sectors at either end.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_IO_BLOCK_H
#define NXU_VFS_BTRFS_BTRFS_IO_BLOCK_H

#include <vfs/btrfs/btrfs_io.h>

#include <drivers/block/block_device.h>

typedef struct {
	block_device_t device;
} btrfs_block_reader_t;

/* Fill *reader for `device`. `storage` must outlive the reader. */
void btrfs_block_reader_init(btrfs_block_reader_t *storage, btrfs_reader_t *reader, block_device_t device);

#endif
