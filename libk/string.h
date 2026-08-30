#ifndef NXU_STRING_H
#define NXU_STRING_H

#include <stddef.h>

void *memset(void *destination, int value, size_t size);
void *memcpy(void *destination, const void *source, size_t size);
int memcmp(const void *left, const void *right, size_t size);
int strcmp(const char *left, const char *right);
size_t strlen(const char *string);

#endif
