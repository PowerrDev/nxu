/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/virtio_stubs.c
 *
 * The parts of the shared VirtIO core the legacy PC has no use for.
 *
 * virtio_init() still scans the platform's VirtIO-MMIO frames; the PC has none
 * (platform->virtio_mmio_count is zero), so the MMIO probe is never asked
 * anything and virtio_mmio.c, which is written for the GIC, is not built.
 *
 * The virtio-gpu entry points below are weak: makedefs/i386/devices.mk always
 * links the real drivers/virtio/virtio_gpu.c alongside this file, so these
 * definitions are dead code there. They stay as a safety net for any other
 * i386 build fragment that links virtio.c (which calls virtio_gpu_attach())
 * without also linking virtio_gpu.c -- reporting "not attached" rather than
 * failing to link.
 */

#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_mmio.h>

#include <stdbool.h>
#include <stdint.h>

/* Weak: a build that does link the real drivers takes theirs. */
#define STUB_WEAK __attribute__((weak))

STUB_WEAK bool virtio_mmio_probe(
	const platform_region_t *region,
	uint32_t intid,
	uint32_t irq_flags,
	virtio_mmio_device_t *device
)
{
	(void)region;
	(void)intid;
	(void)irq_flags;
	(void)device;
	return false;
}

STUB_WEAK bool virtio_gpu_attach(const virtio_device_t *transport)
{
	(void)transport;
	return false;
}

STUB_WEAK uint32_t virtio_gpu_device_count(void)
{
	return 0U;
}

STUB_WEAK void virtio_gpu_dump(void)
{
}
