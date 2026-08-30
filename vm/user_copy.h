#ifndef NXU_VM_USER_COPY_H
#define NXU_VM_USER_COPY_H

#include <stdbool.h>
#include <stdint.h>

bool vm_copy_from_user(
	void *destination,
	uint64_t user_address,
	uint64_t size
);

bool vm_copy_string_from_user(
	char *destination,
	uint64_t user_address,
	uint64_t capacity
);

bool vm_copy_to_user(
	uint64_t user_address,
	const void *source,
	uint64_t size
);

#endif
