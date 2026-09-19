/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io.c
 *
 * Reader plumbing shared by every environment plus the in-memory reader.
 */

#include "btrfs_io.h"

#include <string.h>

static bool btrfs_mem_read(void *ctx, uint64_t offset, void *buffer, size_t length)
{
	const btrfs_mem_reader_t *mem = ctx;

	if (offset > mem->size || length > mem->size - offset) return false;
	memcpy(buffer, mem->data + (size_t)offset, length);
	return true;
}

void btrfs_mem_reader_init(btrfs_mem_reader_t *mem, btrfs_reader_t *reader, const uint8_t *data, uint64_t size)
{
	mem->data = data;
	mem->size = size;
	reader->ctx = mem;
	reader->size = size;
	reader->read = btrfs_mem_read;
}

bool btrfs_reader_read(const btrfs_reader_t *reader, uint64_t offset, void *buffer, size_t length)
{
	if (reader == 0 || reader->read == 0 || buffer == 0) return false;
	if (length == 0U) return true;
	if (offset > reader->size || (uint64_t)length > reader->size - offset) return false;
	return reader->read(reader->ctx, offset, buffer, length);
}
