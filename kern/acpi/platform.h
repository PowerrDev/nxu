/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/acpi/platform.h
 *
 * Declares ACPI platform discovery and initialization interfaces.
 */
#ifndef NXU_ACPI_PLATFORM_H
#define NXU_ACPI_PLATFORM_H

#include <stdint.h>
#include <kern/i386/boot_info.h>

void nxu_acpi_discover(const i386_boot_info_t *boot);
const void *nxu_acpi_find_table(const char *signature);
#endif
