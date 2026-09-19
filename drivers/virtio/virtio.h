#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_H

#include <platform/platform.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool input;
	bool block;
	bool gpu;
	/*
	 * Optional: invoked the instant a GPU attaches, mid-scan, before the
	 * remaining MMIO slots (input, block) are probed. Lets a caller paint
	 * the first frame (e.g. the boot splash) as early as physically
	 * possible instead of waiting for the whole bus scan to finish.
	 */
	void (*on_gpu_ready)(void);
} virtio_probe_policy_t;

/*
 * virtio_init:
 *
 * Probe the VirtIO-MMIO transports discovered by the platform layer and bind
 * supported device drivers.
 *
 * The platform layer owns discovery of MMIO regions and interrupt wiring.
 * The VirtIO core owns transport identification and driver matching. Device
 * semantics remain inside the bound class driver.
 *
 * Unsupported live devices are left untouched and do not fail the scan.
 *
 * Returns true when the complete bus scan finished.
 */
bool virtio_init(
	const platform_t *platform,
	const virtio_probe_policy_t *policy
);

uint32_t virtio_device_count(void);
uint32_t virtio_input_count(void);
uint32_t virtio_block_count(void);
uint32_t virtio_gpu_count(void);

void virtio_dump(void);

#endif
