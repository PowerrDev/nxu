# =============================================================================
# i386 devices: platform discovery, PCI, VirtIO-PCI, block and input drivers
# =============================================================================
#
#   make test-i386-devices   boot the kernel under QEMU with virtio-blk,
#                            keyboard and mouse PCI devices and check the
#                            platform and drivers self-tests
#
# Included from makedefs/i386.mk. The shared block, input, GPU and VirtIO
# sources are the ones the arm64 kernel builds; only virtio_mmio.c (GIC
# specific, the PC has no VirtIO-MMIO devices) is not built here,
# platform/i386/virtio_stubs.c stands in for what the VirtIO core calls from
# it. The pmm/vmm/irq entry points come from the VM and interrupts areas now,
# so devices_shim.c (weak stand-ins for them) is no longer linked.

I386_C_SOURCES += \
    drivers/block/block_device.c \
    drivers/block/partition.c \
    drivers/input/input.c \
    drivers/input/keyboard.c \
    drivers/input/mouse.c \
    drivers/tep/tep_boot.c \
    drivers/tep/tep_crypto.c \
    drivers/tep/tep_mailbox.c \
    drivers/tep/tep_uart_16550.c \
    drivers/video/display.c \
    drivers/virtio/virtio.c \
    drivers/virtio/virtio_block.c \
    drivers/virtio/virtio_gpu.c \
    drivers/virtio/virtio_input.c \
    drivers/virtio/virtio_pci.c \
    drivers/virtio/virtio_sound.c \
    drivers/virtio/virtio_sound_core.c \
    drivers/virtio/virtio_sound_dev.c \
    drivers/virtio/virtio_sound_pcm.c \
    drivers/virtio/virtqueue.c \
    kern/console/ioregistry.c \
    platform/i386/devices_init.c \
    platform/i386/pci.c \
    platform/i386/platform.c \
    platform/i386/rtc.c \
    platform/i386/virtio_stubs.c

.PHONY: test-i386-devices

test-i386-devices: $(I386_KERNEL)

	tools/test_i386_devices.sh $(I386_KERNEL)
