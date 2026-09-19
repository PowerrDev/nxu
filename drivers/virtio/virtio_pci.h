#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_PCI_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_PCI_H

#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_transport.h>
#include <platform/i386/pci.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_PCI_VENDOR_ID 0x1AF4U
#define VIRTIO_PCI_DEVICE_ID_LEGACY_FIRST 0x1000U
#define VIRTIO_PCI_DEVICE_ID_LEGACY_LAST 0x103FU
#define VIRTIO_PCI_DEVICE_ID_MODERN_FIRST 0x1040U
#define VIRTIO_PCI_DEVICE_ID_MODERN_LAST 0x107FU

extern const virtio_transport_ops_t virtio_pci_legacy_ops;
extern const virtio_transport_ops_t virtio_pci_modern_ops;

/*
 * virtio_pci_probe:
 *
 * Identify one enumerated PCI function as a VirtIO device and build the
 * virtio_device_t for it. A device with a legacy (transitional) ID and an
 * I/O-port BAR0 is driven through the legacy register block; a modern-only
 * device (VirtIO input and GPU have no legacy interface) through the
 * capabilities its memory BARs describe, which needs pci_map_bar() to work.
 * Enables I/O or memory decoding and bus mastering, and reads nothing else of
 * the device: status is untouched until the class driver begins.
 *
 * Returns false when the function is not a usable VirtIO device.
 */
bool virtio_pci_probe(const pci_device_t *pci, virtio_device_t *device);

/*
 * virtio_pci_scan:
 *
 * The bus scanner virtio_init() runs: walk the enumerated PCI functions,
 * probe each and bind its class driver.
 */
bool virtio_pci_scan(const platform_t *platform, const virtio_probe_policy_t *policy);

/* Register virtio_pci_scan with virtio_init(). Call before virtio_init(). */
bool virtio_pci_register(void);

/*
 * Interrupt delivery. By default the PCI transports are polled: the block
 * driver spins on the used ring and virtio_input_service() must be called
 * periodically. When enabled, class drivers bind their handlers to the
 * device's PCI interrupt line (a PIC IRQ number) through irq_register(), which
 * must accept several handlers on one line as INTx lines are shared.
 */
void virtio_pci_set_irq_mode(bool enabled);
bool virtio_pci_irq_mode(void);

#endif
