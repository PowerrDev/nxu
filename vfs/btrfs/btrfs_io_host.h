/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io_host.h
 *
 * Host-only: a btrfs_reader_t over a file (a disk image) and an environment
 * built on malloc/free that counts live allocations so tests can prove the
 * core frees everything. Never compiled into a kernel.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_IO_HOST_H
#define NXU_VFS_BTRFS_BTRFS_IO_HOST_H

#include "btrfs_io.h"

typedef struct {
	int fd;
	uint64_t size;
} btrfs_host_file_reader_t;

/* Open path read-only. Returns false (and leaves *file unusable) on failure. */
bool btrfs_host_file_reader_open(btrfs_host_file_reader_t *file, btrfs_reader_t *reader, const char *path);
void btrfs_host_file_reader_close(btrfs_host_file_reader_t *file);

typedef struct {
	uint64_t live_allocations;
	uint64_t live_bytes;
	uint64_t total_allocations;
	uint64_t fail_after;       /* fail the Nth allocation from now (0: never) */
	bool quiet;                /* drop log lines */
	bool verbose;              /* print log lines to stderr */
} btrfs_host_env_t;

void btrfs_host_env_init(btrfs_host_env_t *host, btrfs_env_t *env);

#endif
