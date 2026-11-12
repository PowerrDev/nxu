/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/nbl.h
 *
 * NBL, the NXU Boot Loader: what its source files share.
 *
 * NBL is one UEFI application built per firmware architecture. The common
 * part (main.c) reads the kernel, the boot arguments and the boot chime from
 * the boot volume; an architecture back end places the kernel image and, once
 * boot services are gone, enters it:
 *
 *   x86.c      x64 firmware: the i386 Multiboot kernel or the x86-64 kernel
 *   arm64.c    AArch64 firmware: the arm64 kernel, handed the Device Tree
 *
 * The boot chime is played by the loader itself (chime.c) on the VirtIO Sound
 * device, reached through a transport the architecture supplies
 * (virtio_pci.c or virtio_mmio.c).
 */
#ifndef NXU_BOOT_UEFI_NBL_H
#define NXU_BOOT_UEFI_NBL_H

#include "efi.h"

#include <stdbool.h>

/* Room for the boot arguments, their terminator included. */
#define NBL_BOOTARGS_MAX 2048U

/* The boot argument that tells the kernel how many milliseconds of chime the loader left playing. */
#define NBL_CHIME_BOOTARG "nbl.chime"

extern boot_services *bs;
extern system_table *st;

void nbl_copy(void *destination, const void *source, uintn size);
void nbl_zero(void *destination, uintn size);
int nbl_equal(const void *a, const void *b, uintn size);
uintn nbl_length(const char *string);

/* One status line on the firmware console, before the handoff. */
void nbl_log(const char *message);
void nbl_log_hex(const char *label, uint64_t value);

/* The firmware configuration table published under vendor, or 0. */
void *nbl_config_table(const guid *vendor);

/* One bounded file of the boot volume into pool memory the caller frees. */
status nbl_read_file(handle image, char16 *path, void **out, uintn *size);

/* Whether the space-separated boot arguments hold exactly this word. */
bool nbl_bootarg_present(const char *arguments, const char *word);

/*
 * The architecture back end.
 *
 * nbl_arch_load   validate the kernel image and place its segments at their
 *                 physical addresses
 * nbl_arch_enter  build the handoff, leave boot services and enter the
 *                 kernel; returns only on failure
 */
status nbl_arch_load(const uint8_t *kernel, uintn size);
status nbl_arch_enter(handle image, const char *bootargs);

/*
 * nbl_chime_play:
 *
 * Queue the whole of \NXU\chime.wav on the VirtIO Sound device and start it.
 * The device keeps playing from memory the loader leaves reserved, so the
 * sound carries on while the kernel starts. Returns the length of the chime
 * in milliseconds, or 0 when nothing is playing (no file, no device, or the
 * device refused); the device is then left reset. The memory the device plays
 * from is reported through nbl_chime_region.
 */
uint32_t nbl_chime_play(handle image);
void nbl_chime_region(uint64_t *base, uint64_t *size);

/*
 * The VirtIO Sound device, on whichever bus this firmware has. The transport
 * only finds the device and moves registers; chime.c drives it.
 */
typedef struct {
	/* PCI: the windows the vendor capabilities describe. MMIO: the register frame. */
	volatile uint8_t *common;
	volatile uint8_t *notify;
	volatile uint8_t *config;
	uint32_t notify_multiplier;
} nbl_virtio;

bool nbl_virtio_find_sound(nbl_virtio *device);
uint8_t nbl_virtio_status(nbl_virtio *device);
void nbl_virtio_set_status(nbl_virtio *device, uint8_t value);
uint64_t nbl_virtio_device_features(nbl_virtio *device);
void nbl_virtio_set_driver_features(nbl_virtio *device, uint64_t features);
uint16_t nbl_virtio_queue_max(nbl_virtio *device, uint16_t queue);
void nbl_virtio_queue_enable(nbl_virtio *device, uint16_t queue, uint16_t size, uint64_t descriptors, uint64_t driver,
							 uint64_t device_area);
void nbl_virtio_queue_notify(nbl_virtio *device, uint16_t queue);
uint32_t nbl_virtio_config32(nbl_virtio *device, uint32_t offset);

#endif
