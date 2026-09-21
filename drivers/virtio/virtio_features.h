#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_FEATURES_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_FEATURES_H

/*
 * Feature bits every VirtIO device may offer, whatever its class (VirtIO 1.2
 * section 6). Bits 0..23 and 50..127 belong to the device class; 24..49 are
 * these. Kept apart from the transports so code that only decides about
 * features builds without the kernel.
 */

/* Reserved for legacy devices; QEMU offers both on every modern device. */
#define VIRTIO_F_NOTIFY_ON_EMPTY 24U
#define VIRTIO_F_ANY_LAYOUT 27U
#define VIRTIO_F_RING_INDIRECT_DESC 28U
#define VIRTIO_F_RING_EVENT_IDX 29U
#define VIRTIO_F_VERSION_1 32U
#define VIRTIO_F_ACCESS_PLATFORM 33U
#define VIRTIO_F_RING_PACKED 34U
#define VIRTIO_F_IN_ORDER 35U
#define VIRTIO_F_ORDER_PLATFORM 36U
#define VIRTIO_F_SR_IOV 37U
#define VIRTIO_F_NOTIFICATION_DATA 38U
#define VIRTIO_F_NOTIF_CONFIG_DATA 39U
#define VIRTIO_F_RING_RESET 40U

#endif
