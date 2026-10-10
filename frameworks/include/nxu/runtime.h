#ifndef NXU_USER_RUNTIME_H
#define NXU_USER_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>

/* One anonymous mapping per allocation, reclaimed on free. Alignment must
 * be a power of two <= 64 KiB. NULL/zero failure never discards old realloc. */
void *nxu_alloc(uint64_t size, uint64_t alignment);
bool nxu_free(void *pointer);
void *nxu_realloc(void *pointer, uint64_t size, uint64_t alignment);
_Noreturn void nxu_runtime_abort(void);
#endif
