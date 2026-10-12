/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_KERN_UPDATE_H
#define NXU_KERN_UPDATE_H

#include <stdbool.h>

/* Run while system/recovery volumes are mounted, before any user process starts. */
bool boot_update_install(void);
bool boot_update_is_active(void);
const char *boot_update_recovery_program(void);
void boot_update_watch_start(void);

#endif
