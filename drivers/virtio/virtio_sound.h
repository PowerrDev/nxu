#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_H

#include <drivers/virtio/virtio_transport.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_SND_MAX_DEVICES 1U

/*
 * virtio_snd_attach:
 *
 * Attach the VirtIO Sound driver to one VirtIO device on any transport
 * (VirtIO-MMIO on arm64, VirtIO-PCI on x86).
 *
 * Returns true when the device is bound and registered. A device the driver
 * cannot use (no VERSION_1, a queue that will not start) is reported and
 * skipped: sound is never a reason to stop the bus scan or the boot.
 */
bool virtio_snd_attach(const virtio_device_t *transport);

/*
 * virtio_snd_probe:
 *
 * Report the outcome of the bus scan: how many sound devices were bound, or
 * plainly that there is none. Called once the scan is over.
 */
void virtio_snd_probe(void);

uint32_t virtio_snd_device_count(void);
void virtio_snd_dump(void);

#endif
