/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/startup.h
 *
 * Declares the kernel i386 startup interfaces and data structures.
 */
#ifndef NXU_I386_STARTUP_H
#define NXU_I386_STARTUP_H

#include <stdbool.h>

bool i386_startup_tests_enabled(void);
bool i386_startup_core_test(void);
bool i386_startup_storage_test(void);
#endif
