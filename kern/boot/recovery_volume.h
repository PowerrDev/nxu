/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_KERN_BOOT_RECOVERY_VOLUME_H
#define NXU_KERN_BOOT_RECOVERY_VOLUME_H

#include <drivers/block/block_device.h>
#include <stdbool.h>

/* Named GPT partitions are discovered automatically; raw system disks remain supported. */
block_device_t boot_system_volume_device(void);
bool boot_recovery_volume_available(void);
const char *boot_recovery_volume_program(void);
void boot_mount_recovery_volume(void);

#endif
