/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <drivers/block/file_image.h>
#include <libk/string.h>
#include <vfs/vfs.h>

static bool
file_image_read(block_device_t device, uint64_t sector, uint32_t count, void *buffer)
{
	block_file_image_t *image = device->driver_data;
	if (image == 0 || image->vnode == 0) {
		return false;
	}
	uint64_t bytes = (uint64_t)count * BLOCK_SECTOR_SIZE;
	uint64_t complete = 0;
	while (complete < bytes) {
		uint64_t read = 0;
		if (vnode_read(image->vnode, sector * BLOCK_SECTOR_SIZE + complete, (char *)buffer + complete, bytes - complete,
					   &read) != VFS_STATUS_OK ||
			read == 0 || read > bytes - complete) {
			return false;
		}
		complete += read;
	}
	return true;
}

static const block_device_ops_t g_image_ops = {.read = file_image_read};

bool
block_file_image_open(block_file_image_t *image, const char *path, const char *name)
{
	if (image->vnode != 0 || strlen(name) > BLOCK_DEVICE_NAME_MAX) {
		return false;
	}
	vnode_t vnode;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) {
		return false;
	}
	if (vnode->v_type != VNODE_TYPE_REGULAR || vnode->v_size == 0 || (vnode->v_size % BLOCK_SECTOR_SIZE) != 0) {
		vnode_rele(vnode);
		return false;
	}
	memset(image, 0, sizeof(*image));
	image->vnode = vnode;
	image->device.sector_count = vnode->v_size / BLOCK_SECTOR_SIZE;
	image->device.sector_size = BLOCK_SECTOR_SIZE;
	image->device.logical_block_size = BLOCK_SECTOR_SIZE;
	image->device.read_only = true;
	/* Published only to its owning mount, not the physical-device registry. */
	image->device.registered = true;
	image->device.device_id = UINT32_MAX;
	image->device.ops = &g_image_ops;
	image->device.driver_data = image;
	memcpy(image->device.name, name, strlen(name) + 1);
	return true;
}

void
block_file_image_close(block_file_image_t *image)
{
	if (image->vnode != 0) {
		vnode_rele(image->vnode);
	}
	memset(image, 0, sizeof(*image));
}
