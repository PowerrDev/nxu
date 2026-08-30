#include <crc32c.h>

#include <stddef.h>
#include <stdint.h>

#define CRC32C_POLYNOMIAL 0x82F63B78U

/*
 * crc32c:
 *
 * Extend one reflected CRC32C value. The table-free implementation keeps the
 * freestanding library small; hardware acceleration can replace it without
 * changing filesystem callers.
 */
uint32_t crc32c(uint32_t crc, const void *buffer, size_t size)
{
	const uint8_t *bytes = buffer;

	for (size_t index = 0U; index < size; index++) {
		crc ^= bytes[index];

		for (uint32_t bit = 0U; bit < 8U; bit++) {
			uint32_t mask = 0U - (crc & 1U);
			crc = (crc >> 1U) ^ (CRC32C_POLYNOMIAL & mask);
		}
	}

	return crc;
}
