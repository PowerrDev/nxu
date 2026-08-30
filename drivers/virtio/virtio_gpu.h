#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_GPU_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_GPU_H

#include <drivers/virtio/virtio_mmio.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_GPU_MAX_DEVICES 1U

bool virtio_gpu_attach(const virtio_mmio_device_t *transport);
uint32_t virtio_gpu_device_count(void);
void virtio_gpu_dump(void);

#endif
