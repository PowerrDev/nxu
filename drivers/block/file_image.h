/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_BLOCK_FILE_IMAGE_H
#define NXU_BLOCK_FILE_IMAGE_H

#include <drivers/block/block_device.h>
#include <vfs/vnode.h>

typedef struct {
	struct block_device device;
	vnode_t vnode;
} block_file_image_t;

/* Private, read-only block device. Close only after its filesystem is unmounted. */
bool block_file_image_open(block_file_image_t *image, const char *path, const char *name);
void block_file_image_close(block_file_image_t *image);

#endif
