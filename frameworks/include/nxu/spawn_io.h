/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_USER_SPAWN_IO_H
#define NXU_USER_SPAWN_IO_H

#include <stdint.h>

/* i386 spawn bootstrap: arguments followed by a bidirectional stdio stream. */
int64_t nxu_spawn_io(const char *path, const char *const *arguments, int64_t *stream);
uint64_t nxu_spawn_io_start(const char *const **arguments);

#endif
