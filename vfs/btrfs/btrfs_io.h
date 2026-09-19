/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io.h
 *
 * What the pure Btrfs core needs from its environment, injected so the same
 * code runs in the kernel and natively on the host:
 *
 *   btrfs_env_t     allocator and log sink
 *   btrfs_reader_t  read N bytes at a byte offset of the device
 *
 * Reader implementations:
 *   btrfs_io.c        an in-memory image (host tests, corruption sweeps)
 *   btrfs_io_host.c   a file on the host (POSIX; host builds only)
 *   btrfs_io_block.c  a kernel block_device_t (kernel builds only)
 */

#ifndef NXU_VFS_BTRFS_BTRFS_IO_H
#define NXU_VFS_BTRFS_BTRFS_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * alloc returns zero-filled memory of the given size or NULL. release accepts
 * pointers returned by alloc. log receives one finished line without a
 * trailing newline; it may be NULL to silence the driver.
 */
typedef struct btrfs_env {
	void *ctx;
	void *(*alloc)(void *ctx, size_t size);
	void (*release)(void *ctx, void *pointer);
	void (*log)(void *ctx, const char *line);
} btrfs_env_t;

/*
 * read fills exactly length bytes from byte offset offset, or returns false.
 * The core never asks for a range beyond size, but the reader must still
 * reject one. Any offset and length are legal: the implementation deals with
 * sector alignment.
 */
typedef struct btrfs_reader {
	void *ctx;
	uint64_t size;
	bool (*read)(void *ctx, uint64_t offset, void *buffer, size_t length);
} btrfs_reader_t;

/* A reader over a byte range in memory. The caller owns data and mem. */
typedef struct {
	const uint8_t *data;
	uint64_t size;
} btrfs_mem_reader_t;

void btrfs_mem_reader_init(btrfs_mem_reader_t *mem, btrfs_reader_t *reader, const uint8_t *data, uint64_t size);

/* Bounds-checked wrapper the core uses for every device read. */
bool btrfs_reader_read(const btrfs_reader_t *reader, uint64_t offset, void *buffer, size_t length);

#endif
