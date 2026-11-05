/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/x86_64/memory_map.h
 *
 * Declares the UEFI memory-map conversion used by the x86-64 VM bootstrap.
 */
#ifndef NXU_PLATFORM_X86_64_MEMORY_MAP_H
#define NXU_PLATFORM_X86_64_MEMORY_MAP_H

#include <boot/handoff.h>
#include <platform/platform.h>

#include <stdbool.h>

/*
 * Routine:     x86_platform_memory_map_load
 * Purpose:
 *              Convert the loader's usable ranges to platform_t RAM regions
 *              bounded by the initial x86-64 direct map.
 */
bool x86_platform_memory_map_load(const struct nxu_boot_x86_64 *boot, platform_t *platform);

#endif
