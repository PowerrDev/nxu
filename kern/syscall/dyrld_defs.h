/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/syscall/dyrld_defs.h
 *
 * Defines the kernel syscall ABI data structures and constants.
 */
#ifndef NXU_DYRLD_DEFS_H
#define NXU_DYRLD_DEFS_H
#include <stdint.h>
#define NXU_DYRLD_IMAGES 16U
#define NXU_DYRLD_SEGMENTS 16U
#define NXU_DYRLD_PATH 256U
#define NXU_DYRLD_MAIN_PATH 1U
#define NXU_DYRLD_LOAD 2U
#define NXU_DYRLD_FINALIZE 3U
#define NXU_DYRLD_INTERPRETER "/System/Library/CoreServices/dyrld-runtime"
#define NXU_DYRLD_FRAMEWORKS "/Volumes/mainOS/System/Library/Frameworks/"

typedef struct {
	uint64_t address, size;
	uint32_t flags, reserved;
} nxu_dyrld_segment_t;

typedef struct {
	uint64_t bias, entry, dynamic, dynamic_size;
	uint32_t segment_count, reserved;
	uint8_t identity[32];
	char path[NXU_DYRLD_PATH];
	nxu_dyrld_segment_t segments[NXU_DYRLD_SEGMENTS];
} nxu_dyrld_image_t;

/* LOAD takes an input path and returns validated mappings for this process.
 * FINALIZE takes argument zero, seals recorded GNU_RELRO pages before
 * initializers and closes further LOAD requests until exec. It is idempotent.
 * No kernel pointer crosses the ABI. Images remain mapped until exec/exit. */
#endif
