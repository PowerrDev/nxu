#ifndef NXU_USER_STRING_H
#define NXU_USER_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t nxu_strlen(const char *string);
bool nxu_streq(const char *left, const char *right);
bool nxu_arg_present(const char *arguments, const char *argument);

/* Freestanding builtins, already implemented in frameworks/lib/string.c and
 * linked into every userland binary via USER_COMMON_OBJECTS -- declared
 * here (rather than left implicit) since -ffreestanding disables clang's
 * usual builtin recognition of these names. */
void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);

#endif
