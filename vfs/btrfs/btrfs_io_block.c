/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_io_block.c
 *
 * Kernel block-device reader. See btrfs_io_block.h.
 *
 * Sector arithmetic uses shifts and masks: BLOCK_SECTOR_SIZE is 512, and a
 * 64-bit division on i386 goes through libk's slow __udivmoddi4.
 */

#include <vfs/btrfs/btrfs_io_block.h>

#include <stdint.h>
#include <string.h>

#define BTRFS_SECTOR_SHIFT 9U
#define BTRFS_SECTOR_MASK (BLOCK_SECTOR_SIZE - 1U)

static bool btrfs_block_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	btrfs_block_reader_t *storage = context;
	block_device_t device = storage->device;
	uint8_t *out = buffer;
	uint8_t bounce[BLOCK_SECTOR_SIZE];

	while (length != 0U) {
		uint64_t sector = offset >> BTRFS_SECTOR_SHIFT;
		uint32_t within = (uint32_t)(offset & BTRFS_SECTOR_MASK);

		if (within == 0U && length >= BLOCK_SECTOR_SIZE) {
			size_t whole = length >> BTRFS_SECTOR_SHIFT;
			uint32_t count = whole > UINT32_MAX ? UINT32_MAX : (uint32_t)whole;

			if (!block_device_read(device, sector, count, out)) return false;

			size_t bytes = (size_t)count << BTRFS_SECTOR_SHIFT;
			out += bytes;
			offset += bytes;
			length -= bytes;
			continue;
		}

		if (!block_device_read(device, sector, 1U, bounce)) return false;

		size_t available = BLOCK_SECTOR_SIZE - within;
		size_t amount = length < available ? length : available;

		memcpy(out, bounce + within, amount);
		out += amount;
		offset += amount;
		length -= amount;
	}

	return true;
}

void btrfs_block_reader_init(btrfs_block_reader_t *storage, btrfs_reader_t *reader, block_device_t device)
{
	storage->device = device;
	reader->ctx = storage;
	reader->size = device->sector_count << BTRFS_SECTOR_SHIFT;
	reader->read = btrfs_block_read;
}
