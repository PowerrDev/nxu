/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io_host.c
 *
 * Host-only reader and environment. See btrfs_io_host.h.
 */

#include "btrfs_io_host.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool btrfs_host_file_read(void *ctx, uint64_t offset, void *buffer, size_t length)
{
	btrfs_host_file_reader_t *file = ctx;
	uint8_t *out = buffer;

	if (offset > file->size || length > file->size - offset) return false;

	while (length != 0U) {
		ssize_t got = pread(file->fd, out, length, (off_t)offset);

		if (got <= 0) return false;
		out += got;
		offset += (uint64_t)got;
		length -= (size_t)got;
	}

	return true;
}

bool btrfs_host_file_reader_open(btrfs_host_file_reader_t *file, btrfs_reader_t *reader, const char *path)
{
	struct stat st;

	file->fd = open(path, O_RDONLY);
	if (file->fd < 0) return false;

	if (fstat(file->fd, &st) != 0) {
		close(file->fd);
		file->fd = -1;
		return false;
	}

	file->size = (uint64_t)st.st_size;
	reader->ctx = file;
	reader->size = file->size;
	reader->read = btrfs_host_file_read;
	return true;
}

void btrfs_host_file_reader_close(btrfs_host_file_reader_t *file)
{
	if (file->fd >= 0) close(file->fd);
	file->fd = -1;
}

/* Each allocation carries a size header so the free can keep the counters. */
#define BTRFS_HOST_HEADER 16U

static void *btrfs_host_alloc(void *ctx, size_t size)
{
	btrfs_host_env_t *host = ctx;

	if (host->fail_after != 0U && --host->fail_after == 0U) return 0;

	uint8_t *block = calloc(1U, size + BTRFS_HOST_HEADER);
	if (block == 0) return 0;

	*(size_t *)block = size;
	host->live_allocations++;
	host->live_bytes += size;
	host->total_allocations++;
	return block + BTRFS_HOST_HEADER;
}

static void btrfs_host_release(void *ctx, void *pointer)
{
	btrfs_host_env_t *host = ctx;
	uint8_t *block = (uint8_t *)pointer - BTRFS_HOST_HEADER;

	host->live_allocations--;
	host->live_bytes -= *(size_t *)block;
	free(block);
}

static void btrfs_host_log(void *ctx, const char *line)
{
	btrfs_host_env_t *host = ctx;

	if (host->verbose && !host->quiet) fprintf(stderr, "btrfs: %s\n", line);
}

void btrfs_host_env_init(btrfs_host_env_t *host, btrfs_env_t *env)
{
	memset(host, 0, sizeof(*host));
	env->ctx = host;
	env->alloc = btrfs_host_alloc;
	env->release = btrfs_host_release;
	env->log = btrfs_host_log;
}
