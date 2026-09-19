#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_H

#include <drivers/virtio/virtio_transport.h>
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
 * A bus scanner finds VirtIO devices on a bus other than the platform's MMIO
 * frames (VirtIO-PCI on x86), builds a virtio_device_t for each and hands it to
 * virtio_bind_device(). Returns false to stop the scan on a fatal error.
 */
typedef bool (*virtio_bus_scan_t)(
	const platform_t *platform,
	const virtio_probe_policy_t *policy
);

/*
 * virtio_bus_register:
 *
 * Add a bus scanner run by virtio_init() after the MMIO transports. Must be
 * called before virtio_init(). Registering the same scanner twice is harmless.
 */
bool virtio_bus_register(virtio_bus_scan_t scan);

/*
 * virtio_bind_device:
 *
 * Bind the class driver matching a probed device, whatever transport it uses.
 * Returns false when a block or input device fails to attach (the bus scan
 * must stop); unsupported and policy-disabled devices return true.
 */
bool virtio_bind_device(
	const virtio_device_t *device,
	const virtio_probe_policy_t *policy
);

/*
 * virtio_init:
 *
 * Probe the VirtIO-MMIO transports discovered by the platform layer, then any
 * registered bus scanners, and bind supported device drivers.
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
