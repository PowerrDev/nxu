#ifndef NXU_CRC32C_H
#define NXU_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/*
 * crc32c:
 *
 * Extend a Castagnoli CRC with size bytes from buffer.
 *
 * The routine follows the running-checksum convention used by ext4 and JBD2:
 * callers provide the initial or previous CRC value and no final complement
 * is applied. Filesystem metadata normally begins with an initial value of
 * ~0U or with a UUID-derived checksum seed.
 */
uint32_t crc32c(uint32_t crc, const void *buffer, size_t size);

#endif
