#include <nxu/string.h>

#include <stddef.h>

uint64_t
nxu_strlen(const char *string)
{
	uint64_t length = 0ULL;
	if (string == 0) return 0ULL;
	while (string[length] != '\0') length++;
	return length;
}

bool
nxu_streq(const char *left, const char *right)
{
	if (left == 0 || right == 0) return false;

	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	return *left == *right;
}

bool
nxu_arg_present(const char *arguments, const char *argument)
{
	if (arguments == 0 || argument == 0 || argument[0] == '\0') return false;
	uint64_t wanted = nxu_strlen(argument);

	for (uint64_t index = 0ULL; arguments[index] != '\0';) {
		while (arguments[index] == ' ' || arguments[index] == '\t') index++;
		if (arguments[index] == '\0') break;

		uint64_t start = index;
		while (arguments[index] != '\0' && arguments[index] != ' ' && arguments[index] != '\t') index++;
		uint64_t length = index - start;
		if (length != wanted) continue;

		bool equal = true;
		for (uint64_t offset = 0ULL; offset < wanted; offset++) {
			if (arguments[start + offset] == argument[offset]) continue;
			equal = false;
			break;
		}

		if (equal) return true;
	}

	return false;
}

void *
memcpy(void *destination, const void *source, size_t size)
{
	unsigned char *destination_bytes = destination;
	const unsigned char *source_bytes = source;
	for (size_t index = 0U; index < size; index++) destination_bytes[index] = source_bytes[index];
	return destination;
}

/*
 * The compiler may lower a large struct copy to memmove (it did for bootd's job
 * table once service_config_t grew), so a freestanding userland must have one
 * that is correct when the ranges overlap.
 */
void *
memmove(void *destination, const void *source, size_t size)
{
	unsigned char *destination_bytes = destination;
	const unsigned char *source_bytes = source;

	if (destination_bytes == source_bytes) return destination;

	if (destination_bytes < source_bytes) {
		for (size_t index = 0U; index < size; index++) destination_bytes[index] = source_bytes[index];
	} else {
		for (size_t index = size; index > 0U; index--) destination_bytes[index - 1U] = source_bytes[index - 1U];
	}

	return destination;
}

void *
memset(void *destination, int value, size_t size)
{
	unsigned char *bytes = destination;
	for (size_t index = 0U; index < size; index++) bytes[index] = (unsigned char)value;
	return destination;
}
