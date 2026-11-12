/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/fdt.h
 *
 * The little of the flattened Device Tree format NBL needs on AArch64: find
 * the VirtIO-MMIO frames, and write the copy of the tree the kernel is given.
 */
#ifndef NXU_BOOT_UEFI_FDT_H
#define NXU_BOOT_UEFI_FDT_H

#include <stdbool.h>
#include <stdint.h>

/* The tree the firmware publishes, or 0 when it has none (or an invalid one). */
const void *nbl_fdt_firmware(void);

/* The size the blob says it has; 0 when it is not a Device Tree. */
uint32_t nbl_fdt_size(const void *fdt);

/*
 * nbl_fdt_compatible_reg:
 *
 * The address in `reg` of the index-th node whose `compatible` list holds
 * `compatible`. The address is read as two cells, which is what the root of
 * every 64-bit tree NBL meets declares. False past the last such node.
 */
bool nbl_fdt_compatible_reg(const void *fdt, const char *compatible, uint32_t index, uint64_t *address);

/*
 * nbl_fdt_rebuild:
 *
 * Write a copy of the tree into out[0..capacity) with /chosen/bootargs set
 * to `bootargs` and, when reserve_size is not zero, one more entry in the
 * memory reservation block. Returns the size of the copy, 0 when the source
 * is malformed or the copy does not fit.
 */
uint32_t nbl_fdt_rebuild(const void *fdt, void *out, uint32_t capacity, const char *bootargs, uint64_t reserve_base,
						 uint64_t reserve_size);

#endif
