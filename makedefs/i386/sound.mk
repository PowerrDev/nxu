# =============================================================================
# i386 sound: the VirtIO Sound driver on VirtIO-PCI
# =============================================================================
#
#   make test-i386-sound   boot the kernel against QEMU's virtio-sound-pci with
#                          the wav audio backend recording: a tone, the boot
#                          chime and playsound played through /dev/audio0 (with
#                          the device's interrupt bound and polled), the
#                          recording checked sample by sample on the host, and
#                          the boot with no sound device at all
#
# Included by makedefs/i386.mk. The driver's sources are in devices.mk (the
# device is a VirtIO function like the block and input ones), the chime and the
# test in integration.mk. Needs python3 for the recording check.

.PHONY: test-i386-sound

test-i386-sound: $(I386_KERNEL) i386-disk

	tools/test_i386_sound.sh $(I386_KERNEL) $(BUILD_ROOT)/i386/disk.img
