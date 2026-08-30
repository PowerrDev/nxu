#include <string.h>

#include <stddef.h>
#include <stdint.h>

/*
 * memcpy:
 *
 * Copy size bytes from source to destination.
 *
 * The source and destination regions must not overlap. The destination
 * pointer is returned to the caller.
 */
void *memcpy(
	void *destination,
	const void *source,
	size_t size
)
{
	uint8_t *destination_bytes = destination;
	const uint8_t *source_bytes = source;

	for (size_t index = 0; index < size; index++) {
		destination_bytes[index] = source_bytes[index];
	}

	return destination;
}

/*
 * memset:
 *
 * Fill size bytes of destination with the low eight bits of value.
 *
 * The destination pointer is returned to the caller.
 */
void *memset(
	void *destination,
	int value,
	size_t size
)
{
	uint8_t *bytes = destination;
	uint8_t byte = (uint8_t)value;

	for (size_t index = 0; index < size; index++) {
		bytes[index] = byte;
	}

	return destination;
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
