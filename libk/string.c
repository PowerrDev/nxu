#include <string.h>

#include <memops.h>

#include <stddef.h>
#include <stdint.h>

/*
 * memcpy / memmove / memset: a word at a time (libk/memops.h). The regions of
 * a memcpy must not overlap; memmove's may.
 */
void *memcpy(void *destination, const void *source, size_t size)
{
	return nxu_memops_copy_forward(destination, source, size);
}

void *memmove(void *destination, const void *source, size_t size)
{
	return nxu_memops_move(destination, source, size);
}

void *memset(void *destination, int value, size_t size)
{
	return nxu_memops_set(destination, value, size);
}

int memcmp(const void *left, const void *right, size_t size)
{
	const uint8_t *left_bytes = left;
	const uint8_t *right_bytes = right;

	for (size_t index = 0; index < size; index++) {
		if (left_bytes[index] == right_bytes[index]) continue;
		return left_bytes[index] < right_bytes[index] ? -1 : 1;
	}

	return 0;
}

int strcmp(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	return (unsigned char)*left - (unsigned char)*right;
}

size_t strlen(const char *string)
{
	size_t length = 0U;
	while (string[length] != '\0') length++;
	return length;
}
