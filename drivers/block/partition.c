#include <drivers/block/partition.h>

#include <stdint.h>
#include <string.h>

#define PARTITION_SECTOR_SIZE 512U
#define PARTITION_ALIGNMENT_SECTORS 2048ULL
#define MBR_SIGNATURE_OFFSET 510U
#define MBR_ENTRY_OFFSET 446U
#define MBR_ENTRY_SIZE 16U
#define MBR_ENTRY_COUNT 4U
#define MBR_GPT_PROTECTIVE_TYPE 0xEEU
#define MBR_LINUX_TYPE 0x83U
#define MBR_EFI_TYPE 0xEFU
#define GPT_HEADER_LBA 1ULL
#define GPT_HEADER_SIZE 92U
#define GPT_ENTRY_MIN_SIZE 128U
#define GPT_ENTRY_MAX_SIZE 512U
#define GPT_ENTRY_DEFAULT_SIZE 128U
#define GPT_ENTRY_LIMIT 128U
#define GPT_ENTRY_DEFAULT_COUNT 128U
#define GPT_ENTRY_NAME_OFFSET 56U
#define GPT_ENTRY_NAME_BYTES 72U

static const uint8_t g_gpt_signature[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
static const uint8_t g_gpt_linux_filesystem_guid[16] = {
	0xAFU, 0x3DU, 0xC6U, 0x0FU, 0x83U, 0x84U, 0x72U, 0x47U,
	0x8EU, 0x79U, 0x3DU, 0x69U, 0xD8U, 0x47U, 0x7DU, 0xE4U
};

/* Linux root partition (arm64): what an sevOS system volume is published as. */
static const uint8_t g_gpt_arm64_root_guid[16] = {
	0x45U, 0xB0U, 0x21U, 0xB9U, 0xF0U, 0x1DU, 0xC3U, 0x41U,
	0xAFU, 0x44U, 0x4CU, 0x6FU, 0x28U, 0x0DU, 0x3FU, 0xAEU
};

static const uint8_t g_gpt_efi_system_guid[16] = {
	0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
	0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static uint64_t g_partition_guid_counter = 1ULL;

static const uint8_t *
partition_role_guid(block_partition_role_t role)
{
	if (role == BLOCK_PARTITION_ROLE_SYSTEM) return g_gpt_arm64_root_guid;
	if (role == BLOCK_PARTITION_ROLE_EFI) return g_gpt_efi_system_guid;

	return g_gpt_linux_filesystem_guid;
}

static uint8_t
partition_role_mbr_type(block_partition_role_t role)
{
	return role == BLOCK_PARTITION_ROLE_EFI ? MBR_EFI_TYPE : MBR_LINUX_TYPE;
}

typedef struct {
	uint64_t current_lba;
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint64_t table_lba;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t table_crc32;
	uint8_t disk_guid[16];
} partition_gpt_header_t;

typedef struct {
	uint64_t first;
	uint64_t last;
} partition_extent_t;

static uint32_t partition_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0]
		| ((uint32_t)bytes[1] << 8U)
		| ((uint32_t)bytes[2] << 16U)
		| ((uint32_t)bytes[3] << 24U);
}

static uint64_t partition_le64(const uint8_t *bytes)
{
	return (uint64_t)partition_le32(bytes) | ((uint64_t)partition_le32(bytes + 4U) << 32U);
}

static void partition_store_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8U);
	bytes[2] = (uint8_t)(value >> 16U);
	bytes[3] = (uint8_t)(value >> 24U);
}

static void partition_store_le64(uint8_t *bytes, uint64_t value)
{
	partition_store_le32(bytes, (uint32_t)value);
	partition_store_le32(bytes + 4U, (uint32_t)(value >> 32U));
}

static uint64_t partition_align_up(uint64_t value, uint64_t alignment)
{
	if (alignment == 0ULL) return value;

	uint64_t remainder = value % alignment;
	if (remainder == 0ULL) return value;
	if (value > UINT64_MAX - (alignment - remainder)) return UINT64_MAX;

	return value + alignment - remainder;
}

static bool partition_sector(block_device_t device, uint64_t sector, uint8_t bytes[PARTITION_SECTOR_SIZE])
{
	if (device == 0 || bytes == 0 || device->sector_size != PARTITION_SECTOR_SIZE) return false;

	return block_device_read(device, sector, 1U, bytes);
}

static bool partition_sector_write(block_device_t device, uint64_t sector, const uint8_t bytes[PARTITION_SECTOR_SIZE])
{
	if (device == 0 || bytes == 0 || device->sector_size != PARTITION_SECTOR_SIZE) return false;

	return block_device_write(device, sector, 1U, bytes);
}

static bool partition_mbr_valid(const uint8_t sector[PARTITION_SECTOR_SIZE])
{
	return sector[MBR_SIGNATURE_OFFSET] == 0x55U && sector[MBR_SIGNATURE_OFFSET + 1U] == 0xAAU;
}

static uint32_t partition_crc32_update(uint32_t crc, const uint8_t *bytes, uint32_t length)
{
	for (uint32_t index = 0U; index < length; index++) {
		crc ^= bytes[index];

		for (uint32_t bit = 0U; bit < 8U; bit++) {
			crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
		}
	}

	return crc;
}

static uint32_t partition_crc32(const uint8_t *bytes, uint32_t length)
{
	return ~partition_crc32_update(~0U, bytes, length);
}

static bool partition_gpt_header_read(block_device_t device, uint64_t lba, partition_gpt_header_t *header)
{
	if (device == 0 || header == 0 || lba >= device->sector_count) return false;

	uint8_t sector[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, lba, sector)) return false;

	for (uint32_t index = 0U; index < sizeof(g_gpt_signature); index++) {
		if (sector[index] != g_gpt_signature[index]) return false;
	}

	uint32_t revision = partition_le32(sector + 8U);
	uint32_t header_size = partition_le32(sector + 12U);
	uint32_t stored_crc = partition_le32(sector + 16U);
	if (revision != 0x00010000U || header_size < GPT_HEADER_SIZE || header_size > PARTITION_SECTOR_SIZE) return false;

	uint8_t crc_header[PARTITION_SECTOR_SIZE];
	memcpy(crc_header, sector, header_size);
	memset(crc_header + 16U, 0, 4U);
	if (partition_crc32(crc_header, header_size) != stored_crc) return false;

	uint64_t current_lba = partition_le64(sector + 24U);
	uint64_t backup_lba = partition_le64(sector + 32U);
	uint64_t first_usable = partition_le64(sector + 40U);
	uint64_t last_usable = partition_le64(sector + 48U);
	uint64_t table_lba = partition_le64(sector + 72U);
	uint32_t entry_count = partition_le32(sector + 80U);
	uint32_t entry_size = partition_le32(sector + 84U);
	uint32_t table_crc = partition_le32(sector + 88U);

	if (current_lba != lba || backup_lba >= device->sector_count || backup_lba == current_lba) return false;
	if (first_usable > last_usable || last_usable >= device->sector_count) return false;
	if (table_lba == 0ULL || table_lba >= device->sector_count) return false;
	if (entry_count == 0U || entry_count > GPT_ENTRY_LIMIT) return false;
	if (entry_size < GPT_ENTRY_MIN_SIZE || entry_size > GPT_ENTRY_MAX_SIZE) return false;

	uint64_t table_bytes = (uint64_t)entry_count * entry_size;
	uint64_t table_sectors = (table_bytes + PARTITION_SECTOR_SIZE - 1ULL) / PARTITION_SECTOR_SIZE;
	if (table_sectors == 0ULL || table_lba > device->sector_count - table_sectors) return false;

	*header = (partition_gpt_header_t) {
		.current_lba = current_lba,
		.backup_lba = backup_lba,
		.first_usable_lba = first_usable,
		.last_usable_lba = last_usable,
		.table_lba = table_lba,
		.entry_count = entry_count,
		.entry_size = entry_size,
		.table_crc32 = table_crc
	};
	memcpy(header->disk_guid, sector + 56U, sizeof(header->disk_guid));
	return true;
}

static bool partition_gpt_table_crc(block_device_t device, const partition_gpt_header_t *header, uint32_t *crc_out)
{
	if (device == 0 || header == 0 || crc_out == 0) return false;

	uint64_t remaining = (uint64_t)header->entry_count * header->entry_size;
	uint64_t sector_lba = header->table_lba;
	uint32_t crc = ~0U;
	uint8_t sector[PARTITION_SECTOR_SIZE];

	while (remaining != 0ULL) {
		if (!partition_sector(device, sector_lba++, sector)) return false;

		uint32_t count = remaining > PARTITION_SECTOR_SIZE ? PARTITION_SECTOR_SIZE : (uint32_t)remaining;
		crc = partition_crc32_update(crc, sector, count);
		remaining -= count;
	}

	*crc_out = ~crc;
	return true;
}

static bool partition_gpt_header_valid(block_device_t device, uint64_t lba, partition_gpt_header_t *header)
{
	if (!partition_gpt_header_read(device, lba, header)) return false;

	uint32_t table_crc;
	if (!partition_gpt_table_crc(device, header, &table_crc)) return false;

	return table_crc == header->table_crc32;
}

static bool partition_gpt_entry_read(
	block_device_t device,
	const partition_gpt_header_t *header,
	uint32_t entry,
	uint8_t bytes[GPT_ENTRY_MAX_SIZE]
)
{
	if (device == 0 || header == 0 || bytes == 0 || entry >= header->entry_count) return false;

	uint64_t byte_offset = (uint64_t)entry * header->entry_size;
	uint64_t sector_lba = header->table_lba + byte_offset / PARTITION_SECTOR_SIZE;
	uint32_t within = (uint32_t)(byte_offset % PARTITION_SECTOR_SIZE);
	uint8_t first[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, sector_lba, first)) return false;

	uint32_t first_count = PARTITION_SECTOR_SIZE - within;
	if (first_count > header->entry_size) first_count = header->entry_size;
	memcpy(bytes, first + within, first_count);

	if (first_count == header->entry_size) return true;

	uint8_t second[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, sector_lba + 1ULL, second)) return false;
	memcpy(bytes + first_count, second, header->entry_size - first_count);
	return true;
}

static bool partition_gpt_entry_write(
	block_device_t device,
	const partition_gpt_header_t *header,
	uint32_t entry,
	const uint8_t bytes[GPT_ENTRY_MAX_SIZE]
)
{
	if (device == 0 || header == 0 || bytes == 0 || entry >= header->entry_count) return false;

	uint64_t byte_offset = (uint64_t)entry * header->entry_size;
	uint64_t sector_lba = header->table_lba + byte_offset / PARTITION_SECTOR_SIZE;
	uint32_t within = (uint32_t)(byte_offset % PARTITION_SECTOR_SIZE);
	uint8_t first[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, sector_lba, first)) return false;

	uint32_t first_count = PARTITION_SECTOR_SIZE - within;
	if (first_count > header->entry_size) first_count = header->entry_size;
	memcpy(first + within, bytes, first_count);
	if (!partition_sector_write(device, sector_lba, first)) return false;

	if (first_count == header->entry_size) return true;

	uint8_t second[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, sector_lba + 1ULL, second)) return false;
	memcpy(second, bytes + first_count, header->entry_size - first_count);
	return partition_sector_write(device, sector_lba + 1ULL, second);
}

static bool partition_guid_nonzero(const uint8_t *guid)
{
	for (uint32_t index = 0U; index < 16U; index++) {
		if (guid[index] != 0U) return true;
	}

	return false;
}

static bool partition_guid_equal(const uint8_t *left, const uint8_t *right)
{
	for (uint32_t index = 0U; index < 16U; index++) {
		if (left[index] != right[index]) return false;
	}

	return true;
}

static void partition_guid_generate(block_device_t device, uint32_t salt, uint8_t guid[16])
{
	uint64_t first = device->sector_count ^ ((uint64_t)device->device_id << 32U) ^ g_partition_guid_counter++;
	uint64_t second = ((uint64_t)salt << 48U) ^ (first * 0x9E3779B97F4A7C15ULL) ^ 0xA24BAED4963EE407ULL;

	for (uint32_t index = 0U; index < 8U; index++) {
		guid[index] = (uint8_t)(first >> (index * 8U));
		guid[8U + index] = (uint8_t)(second >> (index * 8U));
	}

	guid[7] = (uint8_t)((guid[7] & 0x0FU) | 0x40U);
	guid[8] = (uint8_t)((guid[8] & 0x3FU) | 0x80U);
}

static uint32_t partition_gpt_count(block_device_t device, const partition_gpt_header_t *header)
{
	uint32_t count = 0U;
	uint8_t entry[GPT_ENTRY_MAX_SIZE];

	for (uint32_t index = 0U; index < header->entry_count; index++) {
		if (!partition_gpt_entry_read(device, header, index, entry)) break;
		if (partition_guid_nonzero(entry)) count++;
	}

	return count;
}

bool block_layout_inspect(block_device_t device, block_layout_info_t *info)
{
	if (device == 0 || info == 0) return false;

	*info = (block_layout_info_t) { .scheme = BLOCK_LAYOUT_RAW, .partition_count = 0U };
	uint8_t sector[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, 0ULL, sector)) return false;
	if (!partition_mbr_valid(sector)) return true;

	bool protective = false;
	uint32_t mbr_count = 0U;

	for (uint32_t index = 0U; index < MBR_ENTRY_COUNT; index++) {
		const uint8_t *entry = sector + MBR_ENTRY_OFFSET + index * MBR_ENTRY_SIZE;
		uint8_t type = entry[4];
		uint32_t sectors = partition_le32(entry + 12U);

		if (type == MBR_GPT_PROTECTIVE_TYPE && sectors != 0U) {
			protective = true;
			continue;
		}

		if (type != 0U && sectors != 0U) mbr_count++;
	}

	if (!protective) {
		info->scheme = BLOCK_LAYOUT_MBR;
		info->partition_count = mbr_count;
		return true;
	}

	partition_gpt_header_t header;
	if (!partition_gpt_header_valid(device, GPT_HEADER_LBA, &header)) {
		info->scheme = BLOCK_LAYOUT_MBR;
		info->partition_count = mbr_count;
		return true;
	}

	info->scheme = BLOCK_LAYOUT_GPT;
	info->partition_count = partition_gpt_count(device, &header);
	return true;
}

static bool block_mbr_partition_get(block_device_t device, uint32_t wanted, block_partition_info_t *info)
{
	uint8_t sector[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, 0ULL, sector) || !partition_mbr_valid(sector)) return false;

	uint32_t current = 0U;

	for (uint32_t index = 0U; index < MBR_ENTRY_COUNT; index++) {
		const uint8_t *entry = sector + MBR_ENTRY_OFFSET + index * MBR_ENTRY_SIZE;
		uint32_t sectors = partition_le32(entry + 12U);
		if (entry[4] == 0U || sectors == 0U || entry[4] == MBR_GPT_PROTECTIVE_TYPE) continue;
		if (current++ != wanted) continue;

		*info = (block_partition_info_t) {
			.scheme = BLOCK_LAYOUT_MBR,
			.index = wanted,
			.start_sector = partition_le32(entry + 8U),
			.sector_count = sectors,
			.type = entry[4]
		};
		memcpy(info->name, "Partition", sizeof("Partition"));
		return true;
	}

	return false;
}

static bool block_gpt_partition_get(block_device_t device, uint32_t wanted, block_partition_info_t *info)
{
	partition_gpt_header_t header;
	if (!partition_gpt_header_valid(device, GPT_HEADER_LBA, &header)) return false;

	uint8_t entry[GPT_ENTRY_MAX_SIZE];
	uint32_t current = 0U;

	for (uint32_t index = 0U; index < header.entry_count; index++) {
		if (!partition_gpt_entry_read(device, &header, index, entry)) return false;
		if (!partition_guid_nonzero(entry)) continue;
		if (current++ != wanted) continue;

		uint64_t first = partition_le64(entry + 32U);
		uint64_t last = partition_le64(entry + 40U);
		if (last < first) return false;

		*info = (block_partition_info_t) {
			.scheme = BLOCK_LAYOUT_GPT,
			.index = wanted,
			.start_sector = first,
			.sector_count = last - first + 1ULL,
			.type = 0U
		};

		uint32_t out = 0U;
		uint32_t name_bytes = header.entry_size > GPT_ENTRY_NAME_OFFSET ? header.entry_size - GPT_ENTRY_NAME_OFFSET : 0U;
		if (name_bytes > GPT_ENTRY_NAME_BYTES) name_bytes = GPT_ENTRY_NAME_BYTES;

		for (uint32_t offset = 0U; offset + 1U < name_bytes && out < BLOCK_PARTITION_NAME_MAX; offset += 2U) {
			uint16_t code = (uint16_t)entry[GPT_ENTRY_NAME_OFFSET + offset]
				| ((uint16_t)entry[GPT_ENTRY_NAME_OFFSET + offset + 1U] << 8U);
			if (code == 0U) break;
			info->name[out++] = code >= 32U && code <= 126U ? (char)code : '?';
		}

		if (out == 0U) {
			memcpy(info->name, "Partition", sizeof("Partition"));
		} else {
			info->name[out] = '\0';
		}

		return true;
	}

	return false;
}

bool block_partition_get(block_device_t device, uint32_t index, block_partition_info_t *info)
{
	if (device == 0 || info == 0) return false;

	block_layout_info_t layout;
	if (!block_layout_inspect(device, &layout) || index >= layout.partition_count) return false;
	if (layout.scheme == BLOCK_LAYOUT_GPT) return block_gpt_partition_get(device, index, info);
	if (layout.scheme == BLOCK_LAYOUT_MBR) return block_mbr_partition_get(device, index, info);

	return false;
}

static void partition_extent_insert(partition_extent_t *extents, uint32_t *count, uint32_t capacity, uint64_t first, uint64_t last)
{
	if (extents == 0 || count == 0 || *count >= capacity || last < first) return;

	uint32_t position = *count;
	while (position != 0U && extents[position - 1U].first > first) {
		extents[position] = extents[position - 1U];
		position--;
	}

	extents[position] = (partition_extent_t) { .first = first, .last = last };
	(*count)++;
}

static bool partition_find_space(
	const partition_extent_t *extents,
	uint32_t extent_count,
	uint64_t first_usable,
	uint64_t last_usable,
	uint64_t requested_sectors,
	uint64_t *start_out,
	uint64_t *count_out
)
{
	if (start_out == 0 || count_out == 0 || first_usable > last_usable) return false;

	uint64_t cursor = partition_align_up(first_usable, PARTITION_ALIGNMENT_SECTORS);
	uint64_t best_start = 0ULL;
	uint64_t best_count = 0ULL;

	for (uint32_t index = 0U; index <= extent_count; index++) {
		bool final_gap = index == extent_count;
		bool gap_before_extent = !final_gap && extents[index].first > cursor;
		if (final_gap || gap_before_extent) {
			uint64_t gap_last = final_gap ? last_usable : extents[index].first - 1ULL;
			if (gap_last >= cursor) {
				uint64_t gap_count = gap_last - cursor + 1ULL;

				if (requested_sectors != 0ULL && gap_count >= requested_sectors) {
					*start_out = cursor;
					*count_out = requested_sectors;
					return true;
				}

				if (requested_sectors == 0ULL && gap_count > best_count) {
					best_start = cursor;
					best_count = gap_count;
				}
			}
		}

		if (final_gap) break;
		if (extents[index].last == UINT64_MAX) return false;

		uint64_t next = extents[index].last + 1ULL;
		if (next > cursor) cursor = partition_align_up(next, PARTITION_ALIGNMENT_SECTORS);
		if (cursor > last_usable) break;
	}

	if (requested_sectors == 0ULL && best_count != 0ULL) {
		*start_out = best_start;
		*count_out = best_count;
		return true;
	}

	return false;
}

static bool partition_gpt_header_write(
	block_device_t device,
	const partition_gpt_header_t *header,
	uint32_t table_crc
)
{
	uint8_t sector[PARTITION_SECTOR_SIZE];
	memset(sector, 0, sizeof(sector));
	memcpy(sector, g_gpt_signature, sizeof(g_gpt_signature));
	partition_store_le32(sector + 8U, 0x00010000U);
	partition_store_le32(sector + 12U, GPT_HEADER_SIZE);
	partition_store_le64(sector + 24U, header->current_lba);
	partition_store_le64(sector + 32U, header->backup_lba);
	partition_store_le64(sector + 40U, header->first_usable_lba);
	partition_store_le64(sector + 48U, header->last_usable_lba);
	memcpy(sector + 56U, header->disk_guid, sizeof(header->disk_guid));
	partition_store_le64(sector + 72U, header->table_lba);
	partition_store_le32(sector + 80U, header->entry_count);
	partition_store_le32(sector + 84U, header->entry_size);
	partition_store_le32(sector + 88U, table_crc);
	partition_store_le32(sector + 16U, partition_crc32(sector, GPT_HEADER_SIZE));
	return partition_sector_write(device, header->current_lba, sector);
}

static bool partition_gpt_mirror_matches(
	const partition_gpt_header_t *primary,
	const partition_gpt_header_t *backup
)
{
	if (primary == 0 || backup == 0) return false;
	if (backup->current_lba != primary->backup_lba || backup->backup_lba != primary->current_lba) return false;
	if (backup->first_usable_lba != primary->first_usable_lba || backup->last_usable_lba != primary->last_usable_lba) return false;
	if (backup->entry_count != primary->entry_count || backup->entry_size != primary->entry_size) return false;
	if (backup->table_crc32 != primary->table_crc32) return false;

	return partition_guid_equal(primary->disk_guid, backup->disk_guid);
}

static bool partition_gpt_backup_rebuild(
	block_device_t device,
	const partition_gpt_header_t *primary,
	partition_gpt_header_t *backup
)
{
	uint64_t table_bytes = (uint64_t)primary->entry_count * primary->entry_size;
	uint64_t table_sectors = (table_bytes + PARTITION_SECTOR_SIZE - 1ULL) / PARTITION_SECTOR_SIZE;
	if (table_sectors == 0ULL || primary->backup_lba <= table_sectors) return false;

	*backup = *primary;
	backup->current_lba = primary->backup_lba;
	backup->backup_lba = primary->current_lba;
	backup->table_lba = backup->current_lba - table_sectors;

	uint8_t sector[PARTITION_SECTOR_SIZE];
	for (uint64_t index = 0ULL; index < table_sectors; index++) {
		if (!partition_sector(device, primary->table_lba + index, sector)) return false;
		if (!partition_sector_write(device, backup->table_lba + index, sector)) return false;
	}

	if (!partition_gpt_header_write(device, backup, primary->table_crc32)) return false;
	if (!block_device_flush(device)) return false;

	return partition_gpt_header_valid(device, backup->current_lba, backup)
		&& partition_gpt_mirror_matches(primary, backup);
}

static bool partition_gpt_pair(block_device_t device, partition_gpt_header_t *primary, partition_gpt_header_t *backup)
{
	if (!partition_gpt_header_valid(device, GPT_HEADER_LBA, primary)) return false;

	if (partition_gpt_header_valid(device, primary->backup_lba, backup)
		&& partition_gpt_mirror_matches(primary, backup)) {
		return true;
	}

	return partition_gpt_backup_rebuild(device, primary, backup);
}

static block_partition_status_t partition_gpt_commit_entry(
	block_device_t device,
	partition_gpt_header_t *primary,
	partition_gpt_header_t *backup,
	uint32_t slot,
	const uint8_t entry[GPT_ENTRY_MAX_SIZE]
)
{
	if (!partition_gpt_entry_write(device, backup, slot, entry)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	uint32_t backup_crc;
	if (!partition_gpt_table_crc(device, backup, &backup_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!partition_gpt_header_write(device, backup, backup_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	if (!partition_gpt_entry_write(device, primary, slot, entry)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	uint32_t primary_crc;
	if (!partition_gpt_table_crc(device, primary, &primary_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (primary_crc != backup_crc) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!partition_gpt_header_write(device, primary, primary_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	return BLOCK_PARTITION_STATUS_OK;
}

block_partition_status_t block_layout_initialize_gpt(block_device_t device)
{
	if (device == 0 || device->sector_size != PARTITION_SECTOR_SIZE) return BLOCK_PARTITION_STATUS_INVALID;
	if (device->read_only) return BLOCK_PARTITION_STATUS_READ_ONLY;

	uint64_t table_bytes = (uint64_t)GPT_ENTRY_DEFAULT_COUNT * GPT_ENTRY_DEFAULT_SIZE;
	uint64_t table_sectors = (table_bytes + PARTITION_SECTOR_SIZE - 1ULL) / PARTITION_SECTOR_SIZE;
	uint64_t minimum_sectors = 2ULL * table_sectors + 4ULL;
	if (device->sector_count <= minimum_sectors) return BLOCK_PARTITION_STATUS_NO_SPACE;

	partition_gpt_header_t primary = {
		.current_lba = GPT_HEADER_LBA,
		.backup_lba = device->sector_count - 1ULL,
		.first_usable_lba = 2ULL + table_sectors,
		.last_usable_lba = device->sector_count - table_sectors - 2ULL,
		.table_lba = 2ULL,
		.entry_count = GPT_ENTRY_DEFAULT_COUNT,
		.entry_size = GPT_ENTRY_DEFAULT_SIZE,
		.table_crc32 = 0U
	};
	partition_guid_generate(device, 0U, primary.disk_guid);

	partition_gpt_header_t backup = primary;
	backup.current_lba = primary.backup_lba;
	backup.backup_lba = primary.current_lba;
	backup.table_lba = backup.current_lba - table_sectors;

	uint8_t zero[PARTITION_SECTOR_SIZE];
	memset(zero, 0, sizeof(zero));

	for (uint64_t index = 0ULL; index < table_sectors; index++) {
		if (!partition_sector_write(device, primary.table_lba + index, zero)) return BLOCK_PARTITION_STATUS_IO_ERROR;
		if (!partition_sector_write(device, backup.table_lba + index, zero)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	}

	uint32_t table_crc;
	if (!partition_gpt_table_crc(device, &primary, &table_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	uint8_t mbr[PARTITION_SECTOR_SIZE];
	memset(mbr, 0, sizeof(mbr));
	uint8_t *protective = mbr + MBR_ENTRY_OFFSET;
	protective[4] = MBR_GPT_PROTECTIVE_TYPE;
	partition_store_le32(protective + 8U, 1U);
	uint64_t protective_sectors = device->sector_count - 1ULL;
	partition_store_le32(protective + 12U, protective_sectors > UINT32_MAX ? UINT32_MAX : (uint32_t)protective_sectors);
	mbr[MBR_SIGNATURE_OFFSET] = 0x55U;
	mbr[MBR_SIGNATURE_OFFSET + 1U] = 0xAAU;

	if (!partition_gpt_header_write(device, &backup, table_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	if (!partition_sector_write(device, 0ULL, mbr)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!partition_gpt_header_write(device, &primary, table_crc)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	return BLOCK_PARTITION_STATUS_OK;
}

static block_partition_status_t block_gpt_partition_create(
	block_device_t device,
	uint64_t requested_sectors,
	const char *name,
	block_partition_role_t role,
	block_partition_info_t *info
)
{
	partition_gpt_header_t primary;
	partition_gpt_header_t backup;
	if (!partition_gpt_pair(device, &primary, &backup)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	partition_extent_t extents[GPT_ENTRY_LIMIT];
	uint32_t extent_count = 0U;
	uint32_t free_slot = UINT32_MAX;
	uint8_t entry[GPT_ENTRY_MAX_SIZE];

	for (uint32_t slot = 0U; slot < primary.entry_count; slot++) {
		if (!partition_gpt_entry_read(device, &primary, slot, entry)) return BLOCK_PARTITION_STATUS_IO_ERROR;

		if (!partition_guid_nonzero(entry)) {
			if (free_slot == UINT32_MAX) free_slot = slot;
			continue;
		}

		uint64_t first = partition_le64(entry + 32U);
		uint64_t last = partition_le64(entry + 40U);
		if (last < first || first < primary.first_usable_lba || last > primary.last_usable_lba) {
			return BLOCK_PARTITION_STATUS_INVALID;
		}

		partition_extent_insert(extents, &extent_count, GPT_ENTRY_LIMIT, first, last);
	}

	if (free_slot == UINT32_MAX) return BLOCK_PARTITION_STATUS_NO_SPACE;

	uint64_t start;
	uint64_t sectors;
	if (!partition_find_space(
		extents,
		extent_count,
		primary.first_usable_lba,
		primary.last_usable_lba,
		requested_sectors,
		&start,
		&sectors
	)) {
		return BLOCK_PARTITION_STATUS_NO_SPACE;
	}

	memset(entry, 0, sizeof(entry));
	memcpy(entry, partition_role_guid(role), 16U);
	partition_guid_generate(device, free_slot + 1U, entry + 16U);
	partition_store_le64(entry + 32U, start);
	partition_store_le64(entry + 40U, start + sectors - 1ULL);

	const char *partition_name = name != 0 && name[0] != '\0' ? name : "Untitled";
	uint32_t name_index = 0U;
	while (partition_name[name_index] != '\0' && name_index < BLOCK_PARTITION_NAME_MAX && name_index * 2U < GPT_ENTRY_NAME_BYTES) {
		entry[GPT_ENTRY_NAME_OFFSET + name_index * 2U] = (uint8_t)partition_name[name_index];
		entry[GPT_ENTRY_NAME_OFFSET + name_index * 2U + 1U] = 0U;
		name_index++;
	}

	block_partition_status_t status = partition_gpt_commit_entry(device, &primary, &backup, free_slot, entry);
	if (status != BLOCK_PARTITION_STATUS_OK) return status;

	if (info != 0) {
		block_layout_info_t layout;
		if (!block_layout_inspect(device, &layout) || layout.partition_count == 0U) return BLOCK_PARTITION_STATUS_IO_ERROR;

		for (uint32_t index = 0U; index < layout.partition_count; index++) {
			block_partition_info_t candidate;
			if (!block_partition_get(device, index, &candidate)) return BLOCK_PARTITION_STATUS_IO_ERROR;
			if (candidate.start_sector != start) continue;
			*info = candidate;
			break;
		}
	}

	return BLOCK_PARTITION_STATUS_OK;
}

static block_partition_status_t block_mbr_partition_create(
	block_device_t device,
	uint64_t requested_sectors,
	block_partition_role_t role,
	block_partition_info_t *info
)
{
	if (device->sector_count <= PARTITION_ALIGNMENT_SECTORS) return BLOCK_PARTITION_STATUS_NO_SPACE;
	if (requested_sectors > UINT32_MAX) return BLOCK_PARTITION_STATUS_NO_SPACE;

	uint8_t sector[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, 0ULL, sector)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!partition_mbr_valid(sector)) return BLOCK_PARTITION_STATUS_INVALID;

	partition_extent_t extents[MBR_ENTRY_COUNT];
	uint32_t extent_count = 0U;
	uint32_t free_slot = UINT32_MAX;

	for (uint32_t slot = 0U; slot < MBR_ENTRY_COUNT; slot++) {
		uint8_t *entry = sector + MBR_ENTRY_OFFSET + slot * MBR_ENTRY_SIZE;
		uint32_t count = partition_le32(entry + 12U);

		if (entry[4] == MBR_GPT_PROTECTIVE_TYPE) return BLOCK_PARTITION_STATUS_NOT_SUPPORTED;

		if (entry[4] == 0U || count == 0U) {
			if (free_slot == UINT32_MAX) free_slot = slot;
			continue;
		}

		uint64_t first = partition_le32(entry + 8U);
		uint64_t last = first + count - 1ULL;
		partition_extent_insert(extents, &extent_count, MBR_ENTRY_COUNT, first, last);
	}

	if (free_slot == UINT32_MAX) return BLOCK_PARTITION_STATUS_NO_SPACE;

	uint64_t last_usable = device->sector_count - 1ULL;
	if (last_usable > UINT32_MAX) last_usable = UINT32_MAX;

	uint64_t start;
	uint64_t count;
	if (!partition_find_space(
		extents,
		extent_count,
		PARTITION_ALIGNMENT_SECTORS,
		last_usable,
		requested_sectors,
		&start,
		&count
	)) {
		return BLOCK_PARTITION_STATUS_NO_SPACE;
	}

	if (start > UINT32_MAX || count > UINT32_MAX) return BLOCK_PARTITION_STATUS_NO_SPACE;

	uint8_t *entry = sector + MBR_ENTRY_OFFSET + free_slot * MBR_ENTRY_SIZE;
	memset(entry, 0, MBR_ENTRY_SIZE);
	entry[4] = partition_role_mbr_type(role);
	partition_store_le32(entry + 8U, (uint32_t)start);
	partition_store_le32(entry + 12U, (uint32_t)count);

	if (!partition_sector_write(device, 0ULL, sector)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	if (info != 0) {
		block_layout_info_t layout;
		if (!block_layout_inspect(device, &layout) || layout.partition_count == 0U) return BLOCK_PARTITION_STATUS_IO_ERROR;

		for (uint32_t index = 0U; index < layout.partition_count; index++) {
			block_partition_info_t candidate;
			if (!block_partition_get(device, index, &candidate)) return BLOCK_PARTITION_STATUS_IO_ERROR;
			if (candidate.start_sector != start) continue;
			*info = candidate;
			break;
		}
	}

	return BLOCK_PARTITION_STATUS_OK;
}

block_partition_status_t block_partition_create(
	block_device_t device,
	uint64_t sector_count,
	const char *name,
	block_partition_role_t role,
	block_partition_info_t *info
)
{
	if (device == 0 || device->sector_size != PARTITION_SECTOR_SIZE) return BLOCK_PARTITION_STATUS_INVALID;
	if (device->read_only) return BLOCK_PARTITION_STATUS_READ_ONLY;

	block_layout_info_t layout;
	if (!block_layout_inspect(device, &layout)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (layout.scheme == BLOCK_LAYOUT_GPT) return block_gpt_partition_create(device, sector_count, name, role, info);
	if (layout.scheme == BLOCK_LAYOUT_MBR) return block_mbr_partition_create(device, sector_count, role, info);

	return BLOCK_PARTITION_STATUS_NOT_SUPPORTED;
}

static block_partition_status_t block_gpt_partition_delete(block_device_t device, uint32_t wanted)
{
	partition_gpt_header_t primary;
	partition_gpt_header_t backup;
	if (!partition_gpt_pair(device, &primary, &backup)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	uint8_t entry[GPT_ENTRY_MAX_SIZE];
	uint32_t current = 0U;

	for (uint32_t slot = 0U; slot < primary.entry_count; slot++) {
		if (!partition_gpt_entry_read(device, &primary, slot, entry)) return BLOCK_PARTITION_STATUS_IO_ERROR;
		if (!partition_guid_nonzero(entry)) continue;
		if (current++ != wanted) continue;

		memset(entry, 0, sizeof(entry));
		return partition_gpt_commit_entry(device, &primary, &backup, slot, entry);
	}

	return BLOCK_PARTITION_STATUS_INVALID;
}

static block_partition_status_t block_mbr_partition_delete(block_device_t device, uint32_t wanted)
{
	uint8_t sector[PARTITION_SECTOR_SIZE];
	if (!partition_sector(device, 0ULL, sector) || !partition_mbr_valid(sector)) return BLOCK_PARTITION_STATUS_IO_ERROR;

	uint32_t current = 0U;

	for (uint32_t slot = 0U; slot < MBR_ENTRY_COUNT; slot++) {
		uint8_t *entry = sector + MBR_ENTRY_OFFSET + slot * MBR_ENTRY_SIZE;
		uint32_t count = partition_le32(entry + 12U);
		if (entry[4] == 0U || count == 0U || entry[4] == MBR_GPT_PROTECTIVE_TYPE) continue;
		if (current++ != wanted) continue;

		memset(entry, 0, MBR_ENTRY_SIZE);
		if (!partition_sector_write(device, 0ULL, sector)) return BLOCK_PARTITION_STATUS_IO_ERROR;
		if (!block_device_flush(device)) return BLOCK_PARTITION_STATUS_IO_ERROR;
		return BLOCK_PARTITION_STATUS_OK;
	}

	return BLOCK_PARTITION_STATUS_INVALID;
}

block_partition_status_t block_partition_delete(block_device_t device, uint32_t index)
{
	if (device == 0 || device->sector_size != PARTITION_SECTOR_SIZE) return BLOCK_PARTITION_STATUS_INVALID;
	if (device->read_only) return BLOCK_PARTITION_STATUS_READ_ONLY;

	block_layout_info_t layout;
	if (!block_layout_inspect(device, &layout)) return BLOCK_PARTITION_STATUS_IO_ERROR;
	if (index >= layout.partition_count) return BLOCK_PARTITION_STATUS_INVALID;
	if (layout.scheme == BLOCK_LAYOUT_GPT) return block_gpt_partition_delete(device, index);
	if (layout.scheme == BLOCK_LAYOUT_MBR) return block_mbr_partition_delete(device, index);

	return BLOCK_PARTITION_STATUS_NOT_SUPPORTED;
}
