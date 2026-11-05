/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/cpu_local.c
 *
 * Boot CPU local pointer for the single-processor x86-64 VM milestone.
 */

#include <kern/x86_64/smp.h>

void *g_x86_cpu_local;
