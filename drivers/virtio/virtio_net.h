/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_DRIVERS_VIRTIO_NET_H
#define NXU_DRIVERS_VIRTIO_NET_H

#include <drivers/virtio/virtio_transport.h>

bool virtio_net_attach(const virtio_device_t *transport);

#endif
