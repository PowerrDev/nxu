#include <vfs/jbd2.h>

#include <crc32c.h>
#include <kern/memory/heap.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define JBD2_MAGIC 0xC03B3998U

#define JBD2_DESCRIPTOR_BLOCK 1U
#define JBD2_COMMIT_BLOCK 2U
#define JBD2_SUPERBLOCK_V2 4U

#define JBD2_FEATURE_INCOMPAT_REVOKE 0x00000001U
#define JBD2_FEATURE_INCOMPAT_64BIT 0x00000002U
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 0x00000004U
#define JBD2_FEATURE_INCOMPAT_CSUM_V2 0x00000008U
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 0x00000010U
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT 0x00000020U

#define JBD2_TAG_ESCAPE 0x00000001U
#define JBD2_TAG_SAME_UUID 0x00000002U
#define JBD2_TAG_DELETED 0x00000004U
#define JBD2_TAG_LAST 0x00000008U
#define JBD2_TAG_SUPPORTED_MASK (JBD2_TAG_ESCAPE | JBD2_TAG_SAME_UUID | JBD2_TAG_LAST)

#define JBD2_CHECKSUM_CRC32C 4U
#define JBD2_SUPERBLOCK_BYTES 1024U
#define JBD2_DESCRIPTOR_TAIL_BYTES 4U
#define JBD2_TAG3_BYTES 16U

static uint32_t jbd2_be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24U)
		| ((uint32_t)bytes[1] << 16U)
		| ((uint32_t)bytes[2] << 8U)
		| (uint32_t)bytes[3];
}

static void jbd2_set_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24U);
	bytes[1] = (uint8_t)(value >> 16U);
	bytes[2] = (uint8_t)(value >> 8U);
	bytes[3] = (uint8_t)value;
}

static bool jbd2_uuid_equal(const uint8_t a[16], const uint8_t b[16])
{
	for (uint32_t index = 0U; index < 16U; index++) {
		if (a[index] != b[index]) return false;
	}

	return true;
}

static bool jbd2_read_fs_block(jbd2_t *journal, uint64_t block, void *buffer)
{
	if (block >= journal->filesystem_blocks) return false;
	uint64_t sectors = journal->block_size / BLOCK_SECTOR_SIZE;
	return block_device_read(journal->device, block * sectors, (uint32_t)sectors, buffer);
}

static bool jbd2_write_fs_block(jbd2_t *journal, uint64_t block, const void *buffer)
{
	if (block >= journal->filesystem_blocks) return false;
	uint64_t sectors = journal->block_size / BLOCK_SECTOR_SIZE;
	return block_device_write(journal->device, block * sectors, (uint32_t)sectors, buffer);
}

static bool jbd2_read_log_block(jbd2_t *journal, uint32_t logical, void *buffer)
{
	if (logical >= journal->journal_blocks) return false;
	return jbd2_read_fs_block(journal, journal->journal_block_map[logical], buffer);
}

static bool jbd2_write_log_block(jbd2_t *journal, uint32_t logical, const void *buffer)
{
	if (logical >= journal->journal_blocks) return false;
	return jbd2_write_fs_block(journal, journal->journal_block_map[logical], buffer);
}

static uint32_t jbd2_next_log_block(const jbd2_t *journal, uint32_t block)
{
	block++;
	if (block >= journal->maximum) block = journal->first;
	return block;
}

/*
 * jbd2_is_journal_block:
 *
 * Return true when a filesystem block belongs to the internal journal file.
 * Journal extents need not be physically contiguous.
 */
static bool jbd2_is_journal_block(const jbd2_t *journal, uint64_t block)
{
	for (uint32_t index = 0U; index < journal->journal_blocks; index++) {
		if (journal->journal_block_map[index] == block) return true;
	}

	return false;
}

/*
 * jbd2_superblock_checksum:
 *
 * Compute the CRC32C covering the complete 1024-byte journal superblock with
 * s_checksum cleared. JBD2 superblock fields remain big-endian on disk.
 */
static uint32_t jbd2_superblock_checksum(uint8_t superblock[JBD2_SUPERBLOCK_BYTES])
{
	uint32_t saved = jbd2_be32(superblock + 0xFCU);
	jbd2_set_be32(superblock + 0xFCU, 0U);
	uint32_t checksum = crc32c(~0U, superblock, JBD2_SUPERBLOCK_BYTES);
	jbd2_set_be32(superblock + 0xFCU, saved);
	return checksum;
}

static bool jbd2_write_superblock(jbd2_t *journal, uint32_t start, uint32_t sequence, uint32_t head)
{
	jbd2_set_be32(journal->superblock + 0x18U, sequence);
	jbd2_set_be32(journal->superblock + 0x1CU, start);
	jbd2_set_be32(journal->superblock + 0x58U, head);
	jbd2_set_be32(journal->superblock + 0xFCU, 0U);
	jbd2_set_be32(
		journal->superblock + 0xFCU,
		crc32c(~0U, journal->superblock, JBD2_SUPERBLOCK_BYTES)
	);

	memset(journal->io_block, 0, journal->block_size);
	memcpy(journal->io_block, journal->superblock, JBD2_SUPERBLOCK_BYTES);
	return jbd2_write_log_block(journal, 0U, journal->io_block);
}

static uint32_t jbd2_descriptor_checksum(jbd2_t *journal, uint8_t *block)
{
	uint32_t offset = journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES;
	jbd2_set_be32(block + offset, 0U);
	return crc32c(journal->checksum_seed, block, journal->block_size);
}

static uint32_t jbd2_tag_checksum(jbd2_t *journal, uint32_t sequence, const void *block)
{
	uint8_t sequence_bytes[4];
	jbd2_set_be32(sequence_bytes, sequence);
	uint32_t checksum = crc32c(journal->checksum_seed, sequence_bytes, sizeof(sequence_bytes));
	return crc32c(checksum, block, journal->block_size);
}

static uint32_t jbd2_commit_checksum(jbd2_t *journal, uint8_t *block)
{
	jbd2_set_be32(block + 0x10U, 0U);
	return crc32c(journal->checksum_seed, block, journal->block_size);
}

/*
 * jbd2_enable_checksum_v3:
 *
 * Promote an empty journal to the checksum-v3 format used by NXU.
 *
 * Only an inactive journal may be converted. Existing committed journal
 * contents are never reinterpreted under a different tag format.
 */
static bool jbd2_enable_checksum_v3(
	jbd2_t *journal,
	uint32_t feature_incompat
)
{
	uint32_t start = jbd2_be32(journal->superblock + 0x1CU);

	if (start != 0U) return false;


	if (
		(feature_incompat &
			(JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
			JBD2_FEATURE_INCOMPAT_FAST_COMMIT)) != 0U
	) {
		return false;
	}

	feature_incompat &= ~JBD2_FEATURE_INCOMPAT_CSUM_V2;

	feature_incompat |= JBD2_FEATURE_INCOMPAT_CSUM_V3;

	jbd2_set_be32(
		journal->superblock + 0x28U,
		feature_incompat
	);

	journal->superblock[0x50U] = JBD2_CHECKSUM_CRC32C;

	jbd2_set_be32(
		journal->superblock + 0xFCU,
		0U
	);

	jbd2_set_be32(
		journal->superblock + 0xFCU,
		jbd2_superblock_checksum(journal->superblock)
	);

	memset(
		journal->io_block,
		0,
		journal->block_size
	);

	memcpy(
		journal->io_block,
		journal->superblock,
		JBD2_SUPERBLOCK_BYTES
	);

	if (!jbd2_write_log_block(
		journal,
		0U,
		journal->io_block
	)) {
		return false;
	}

	if (!block_device_flush(journal->device)) {
		return false;
	}

	return true;
}

/*
 * jbd2_init:
 *
 * Validate the journal superblock and allocate transaction storage. The
 * current writer emits checksum-v3 tags and one descriptor block per
 * transaction; the controlled NXU filesystem profile guarantees that this
 * is sufficient for every metadata operation generated by the kernel.
 */
jbd2_status_t jbd2_init(
	jbd2_t *journal,
	block_device_t device,
	uint32_t block_size,
	uint64_t filesystem_blocks,
	const uint64_t *journal_block_map,
	uint32_t journal_blocks,
	const uint8_t expected_uuid[16]
)
{
	if (
		journal == 0 || device == 0 || journal_block_map == 0 || expected_uuid == 0 ||
		block_size < BLOCK_SECTOR_SIZE || (block_size % BLOCK_SECTOR_SIZE) != 0U ||
		journal_blocks < 8U
	) {
		return JBD2_STATUS_INVALID;
	}

	memset(journal, 0, sizeof(*journal));
	journal->device = device;
	journal->block_size = block_size;
	journal->filesystem_blocks = filesystem_blocks;
	journal->journal_blocks = journal_blocks;
	journal->journal_block_map = kmalloc((size_t)journal_blocks * sizeof(*journal->journal_block_map));
	journal->superblock = kmalloc(JBD2_SUPERBLOCK_BYTES);
	journal->io_block = kmalloc(block_size);
	journal->transaction_storage = kmalloc((uint64_t)JBD2_TRANSACTION_MAX_BLOCKS * block_size);
	if (journal->journal_block_map == 0 || journal->superblock == 0 || journal->io_block == 0 || journal->transaction_storage == 0) {
		jbd2_destroy(journal);
		return JBD2_STATUS_NO_MEMORY;
	}

	for (uint32_t index = 0U; index < journal_blocks; index++) {
		if (journal_block_map[index] >= filesystem_blocks) {
			jbd2_destroy(journal);
			return JBD2_STATUS_INVALID;
		}

		journal->journal_block_map[index] = journal_block_map[index];
	}

	memset(journal->io_block, 0, block_size);
	if (!jbd2_read_log_block(journal, 0U, journal->io_block)) {
		jbd2_destroy(journal);
		return JBD2_STATUS_IO_ERROR;
	}
	memcpy(journal->superblock, journal->io_block, JBD2_SUPERBLOCK_BYTES);

	if (
		jbd2_be32(journal->superblock + 0x00U) != JBD2_MAGIC ||
		jbd2_be32(journal->superblock + 0x04U) != JBD2_SUPERBLOCK_V2 ||
		jbd2_be32(journal->superblock + 0x0CU) != block_size
	) {
		jbd2_destroy(journal);
		return JBD2_STATUS_NOT_SUPPORTED;
	}

	uint32_t maximum = jbd2_be32(journal->superblock + 0x10U);
	uint32_t first = jbd2_be32(journal->superblock + 0x14U);
	uint32_t feature_incompat = jbd2_be32(journal->superblock + 0x28U);

	if (maximum == 0U || maximum > journal_blocks || first == 0U || first >= maximum) {
		jbd2_destroy(journal);
		return JBD2_STATUS_IO_ERROR;
	}

	if ((feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) == 0U) {
		if (!jbd2_enable_checksum_v3(
			journal,
			feature_incompat
		)) {
			jbd2_destroy(journal);
			return JBD2_STATUS_NOT_SUPPORTED;
		}

		feature_incompat = jbd2_be32(journal->superblock + 0x28U);
	}

	if ((feature_incompat & (JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT | JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_FAST_COMMIT)) != 0U) {
		jbd2_destroy(journal);
		return JBD2_STATUS_NOT_SUPPORTED;
	}

	if (journal->superblock[0x50U] != JBD2_CHECKSUM_CRC32C) {
		jbd2_destroy(journal);
		return JBD2_STATUS_NOT_SUPPORTED;
	}

	if (!jbd2_uuid_equal(journal->superblock + 0x30U, expected_uuid)) {
		jbd2_destroy(journal);
		return JBD2_STATUS_IO_ERROR;
	}

	if (jbd2_superblock_checksum(journal->superblock) != jbd2_be32(journal->superblock + 0xFCU)) {
		jbd2_destroy(journal);
		return JBD2_STATUS_CHECKSUM;
	}

	journal->first = first;
	journal->maximum = maximum;
	journal->sequence = jbd2_be32(journal->superblock + 0x18U);
	if (journal->sequence == 0U) journal->sequence = 1U;
	journal->head = jbd2_be32(journal->superblock + 0x58U);
	if (journal->head < first || journal->head >= maximum) journal->head = first;
	journal->feature_incompat = feature_incompat;
	journal->checksum_v3 = true;
	journal->block64 = (feature_incompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0U;
	memcpy(journal->uuid, expected_uuid, 16U);
	journal->checksum_seed = crc32c(~0U, journal->uuid, 16U);
	journal->active = true;

	for (uint32_t index = 0U; index < JBD2_TRANSACTION_MAX_BLOCKS; index++) {
		journal->entries[index].data = journal->transaction_storage + (uint64_t)index * block_size;
	}

	return JBD2_STATUS_OK;
}

void jbd2_destroy(jbd2_t *journal)
{
	if (journal == 0) return;

	if (journal->transaction_storage != 0) (void)kfree(journal->transaction_storage);
	if (journal->journal_block_map != 0) (void)kfree(journal->journal_block_map);
	if (journal->io_block != 0) (void)kfree(journal->io_block);
	if (journal->superblock != 0) (void)kfree(journal->superblock);
	memset(journal, 0, sizeof(*journal));
}

jbd2_status_t jbd2_begin(jbd2_t *journal)
{
	if (journal == 0 || !journal->active || journal->transaction_active) return JBD2_STATUS_INVALID;
	journal->entry_count = 0U;
	journal->transaction_active = true;
	return JBD2_STATUS_OK;
}

void jbd2_abort(jbd2_t *journal)
{
	if (journal == 0) return;
	journal->entry_count = 0U;
	journal->transaction_active = false;
}

jbd2_status_t jbd2_stage(jbd2_t *journal, uint64_t home_block, const void *block)
{
	if (journal == 0 || block == 0 || !journal->transaction_active) return JBD2_STATUS_INVALID;

	if (home_block >= journal->filesystem_blocks) return JBD2_STATUS_INVALID;
	if (jbd2_is_journal_block(journal, home_block)) return JBD2_STATUS_INVALID;

	for (uint32_t index = 0U; index < journal->entry_count; index++) {
		if (journal->entries[index].home_block != home_block) continue;
		memcpy(journal->entries[index].data, block, journal->block_size);
		return JBD2_STATUS_OK;
	}

	if (journal->entry_count >= JBD2_TRANSACTION_MAX_BLOCKS) return JBD2_STATUS_NO_SPACE;
	jbd2_transaction_entry_t *entry = &journal->entries[journal->entry_count++];
	entry->home_block = home_block;
	memcpy(entry->data, block, journal->block_size);
	return JBD2_STATUS_OK;
}

bool jbd2_overlay(const jbd2_t *journal, uint64_t home_block, void *block)
{
	if (journal == 0 || block == 0 || !journal->transaction_active) return false;
	for (uint32_t index = 0U; index < journal->entry_count; index++) {
		if (journal->entries[index].home_block != home_block) continue;
		memcpy(block, journal->entries[index].data, journal->block_size);
		return true;
	}
	return false;
}

bool jbd2_is_transaction_active(const jbd2_t *journal)
{
	return journal != 0 && journal->transaction_active;
}

void jbd2_set_crash_after_commit(jbd2_t *journal, bool enabled)
{
	if (journal == 0) return;
	journal->crash_after_commit = enabled;
	if (enabled) journal->crash_point_reached = false;
}

bool jbd2_crash_point_reached(const jbd2_t *journal)
{
	return journal != 0 && journal->crash_point_reached;
}

/*
 * jbd2_build_descriptor:
 *
 * Construct one checksum-v3 descriptor followed by tags for every staged
 * home block. The first tag carries the journal UUID; later tags set
 * SAME_UUID. LAST_TAG terminates parsing without relying on unused bytes.
 */
static bool jbd2_build_descriptor(jbd2_t *journal, uint32_t sequence, uint8_t *descriptor)
{
	memset(descriptor, 0, journal->block_size);
	jbd2_set_be32(descriptor + 0x00U, JBD2_MAGIC);
	jbd2_set_be32(descriptor + 0x04U, JBD2_DESCRIPTOR_BLOCK);
	jbd2_set_be32(descriptor + 0x08U, sequence);

	uint32_t offset = 12U;
	for (uint32_t index = 0U; index < journal->entry_count; index++) {
		bool first = index == 0U;
		bool last = index + 1U == journal->entry_count;
		uint32_t need = JBD2_TAG3_BYTES + (first ? 16U : 0U);
		if (offset > journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES || need > journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES - offset) {
			return false;
		}

		jbd2_transaction_entry_t *entry = &journal->entries[index];
		uint32_t flags = first ? 0U : JBD2_TAG_SAME_UUID;
		if (last) flags |= JBD2_TAG_LAST;
		if (jbd2_be32(entry->data) == JBD2_MAGIC) flags |= JBD2_TAG_ESCAPE;

		jbd2_set_be32(descriptor + offset + 0x00U, (uint32_t)entry->home_block);
		jbd2_set_be32(descriptor + offset + 0x04U, flags);
		jbd2_set_be32(descriptor + offset + 0x08U, (uint32_t)(entry->home_block >> 32U));

		memcpy(journal->io_block, entry->data, journal->block_size);
		if ((flags & JBD2_TAG_ESCAPE) != 0U) jbd2_set_be32(journal->io_block, 0U);
		jbd2_set_be32(descriptor + offset + 0x0CU, jbd2_tag_checksum(journal, sequence, journal->io_block));
		offset += JBD2_TAG3_BYTES;
		if (first) {
			memcpy(descriptor + offset, journal->uuid, 16U);
			offset += 16U;
		}
	}

	jbd2_set_be32(
		descriptor + journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES,
		jbd2_descriptor_checksum(journal, descriptor)
	);
	return true;
}

/*
 * jbd2_commit:
 *
 * Write-ahead ordering is explicit: mark the journal active, write descriptor
 * and metadata images, flush them, write and flush the commit block, then
 * checkpoint home metadata. A debug crash point returns after the durable
 * commit and before checkpointing so the next boot must replay the log.
 */
jbd2_status_t jbd2_commit(jbd2_t *journal)
{
	if (journal == 0 || !journal->active || !journal->transaction_active) return JBD2_STATUS_INVALID;

	if (journal->entry_count == 0U) {
		jbd2_abort(journal);
		return JBD2_STATUS_OK;
	}

	if (journal->entry_count + 2U >= journal->maximum - journal->first) return JBD2_STATUS_NO_SPACE;

	uint32_t sequence = journal->sequence;
	uint32_t descriptor_block = journal->first;
	uint32_t cursor = jbd2_next_log_block(journal, descriptor_block);
	if (!jbd2_write_superblock(journal, descriptor_block, sequence, descriptor_block)) return JBD2_STATUS_IO_ERROR;

	if (!block_device_flush(journal->device)) return JBD2_STATUS_IO_ERROR;

	uint8_t *descriptor = kmalloc(journal->block_size);
	if (descriptor == 0) return JBD2_STATUS_NO_MEMORY;
	bool built = jbd2_build_descriptor(journal, sequence, descriptor);
	if (!built || !jbd2_write_log_block(journal, descriptor_block, descriptor)) {
		(void)kfree(descriptor);
		return built ? JBD2_STATUS_IO_ERROR : JBD2_STATUS_NO_SPACE;
	}
	(void)kfree(descriptor);

	for (uint32_t index = 0U; index < journal->entry_count; index++) {
		jbd2_transaction_entry_t *entry = &journal->entries[index];
		memcpy(journal->io_block, entry->data, journal->block_size);
		if (jbd2_be32(entry->data) == JBD2_MAGIC) jbd2_set_be32(journal->io_block, 0U);
		if (!jbd2_write_log_block(journal, cursor, journal->io_block)) return JBD2_STATUS_IO_ERROR;
		cursor = jbd2_next_log_block(journal, cursor);
	}

	if (!block_device_flush(journal->device)) return JBD2_STATUS_IO_ERROR;

	memset(journal->io_block, 0, journal->block_size);
	jbd2_set_be32(journal->io_block + 0x00U, JBD2_MAGIC);
	jbd2_set_be32(journal->io_block + 0x04U, JBD2_COMMIT_BLOCK);
	jbd2_set_be32(journal->io_block + 0x08U, sequence);
	journal->io_block[0x0CU] = JBD2_CHECKSUM_CRC32C;
	journal->io_block[0x0DU] = 4U;
	jbd2_set_be32(journal->io_block + 0x10U, jbd2_commit_checksum(journal, journal->io_block));
	if (!jbd2_write_log_block(journal, cursor, journal->io_block)) return JBD2_STATUS_IO_ERROR;

	if (!block_device_flush(journal->device)) return JBD2_STATUS_IO_ERROR;

	journal->commit_count++;
	if (journal->crash_after_commit) {
		journal->crash_after_commit = false;
		journal->crash_point_reached = true;
		journal->transaction_active = false;
		journal->entry_count = 0U;
		return JBD2_STATUS_CRASH_POINT;
	}

	for (uint32_t index = 0U; index < journal->entry_count; index++) {
		jbd2_transaction_entry_t *entry = &journal->entries[index];
		if (!jbd2_write_fs_block(journal, entry->home_block, entry->data)) return JBD2_STATUS_IO_ERROR;
	}

	if (!block_device_flush(journal->device)) return JBD2_STATUS_IO_ERROR;

	uint32_t next = jbd2_next_log_block(journal, cursor);
	journal->sequence = sequence + 1U;
	if (journal->sequence == 0U) journal->sequence = 1U;
	journal->head = next;
	if (!jbd2_write_superblock(journal, 0U, journal->sequence, next)) return JBD2_STATUS_IO_ERROR;

	if (!block_device_flush(journal->device)) return JBD2_STATUS_IO_ERROR;

	journal->transaction_active = false;
	journal->entry_count = 0U;
	return JBD2_STATUS_OK;
}

/*
 * jbd2_recover:
 *
 * Recover the single pending transaction shape emitted by NXU. The journal
 * is checkpointed immediately after every normal commit, so at most one
 * committed transaction can remain after a simulated or real power loss.
 */
jbd2_status_t jbd2_recover(jbd2_t *journal)
{
	if (journal == 0 || !journal->active) return JBD2_STATUS_INVALID;
	uint32_t start = jbd2_be32(journal->superblock + 0x1CU);
	if (start == 0U) return JBD2_STATUS_OK;
	uint32_t sequence = jbd2_be32(journal->superblock + 0x18U);
	if (start < journal->first || start >= journal->maximum) return JBD2_STATUS_IO_ERROR;

	uint8_t *descriptor = kmalloc(journal->block_size);
	uint8_t *data_block = kmalloc(journal->block_size);
	if (descriptor == 0 || data_block == 0) {
		if (descriptor != 0) (void)kfree(descriptor);
		if (data_block != 0) (void)kfree(data_block);
		return JBD2_STATUS_NO_MEMORY;
	}

	if (!jbd2_read_log_block(journal, start, descriptor)) goto io_fail;
	uint32_t cursor = start;

	if (
		jbd2_be32(descriptor + 0x00U) != JBD2_MAGIC ||
		jbd2_be32(descriptor + 0x04U) != JBD2_DESCRIPTOR_BLOCK ||
		jbd2_be32(descriptor + 0x08U) != sequence
	) {
		goto discard;
	}
	uint32_t expected_descriptor = jbd2_be32(descriptor + journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES);
	if (jbd2_descriptor_checksum(journal, descriptor) != expected_descriptor) goto checksum_fail;

	uint32_t offset = 12U;
	cursor = jbd2_next_log_block(journal, start);
	uint32_t recovered = 0U;
	bool last = false;
	while (!last) {
		if (offset + JBD2_TAG3_BYTES > journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES) goto checksum_fail;
		uint64_t home = jbd2_be32(descriptor + offset + 0x00U);
		uint32_t flags = jbd2_be32(descriptor + offset + 0x04U);
		if ((flags & ~JBD2_TAG_SUPPORTED_MASK) != 0U) goto checksum_fail;
		if (recovered == 0U && (flags & JBD2_TAG_SAME_UUID) != 0U) goto checksum_fail;
		home |= (uint64_t)jbd2_be32(descriptor + offset + 0x08U) << 32U;
		uint32_t expected_data = jbd2_be32(descriptor + offset + 0x0CU);
		offset += JBD2_TAG3_BYTES;
		if ((flags & JBD2_TAG_SAME_UUID) == 0U) {
			if (offset + 16U > journal->block_size - JBD2_DESCRIPTOR_TAIL_BYTES) goto checksum_fail;
			if (!jbd2_uuid_equal(descriptor + offset, journal->uuid)) goto checksum_fail;
			offset += 16U;
		}

		if (home >= journal->filesystem_blocks || jbd2_is_journal_block(journal, home)) goto checksum_fail;
		if (!jbd2_read_log_block(journal, cursor, data_block)) goto io_fail;
		if (jbd2_tag_checksum(journal, sequence, data_block) != expected_data) goto checksum_fail;
		if ((flags & JBD2_TAG_ESCAPE) != 0U) jbd2_set_be32(data_block, JBD2_MAGIC);
		if (recovered >= JBD2_TRANSACTION_MAX_BLOCKS) goto checksum_fail;
		journal->entries[recovered].home_block = home;
		memcpy(journal->entries[recovered].data, data_block, journal->block_size);
		recovered++;
		cursor = jbd2_next_log_block(journal, cursor);
		last = (flags & JBD2_TAG_LAST) != 0U;
	}

	if (!jbd2_read_log_block(journal, cursor, data_block)) goto io_fail;
	if (
		jbd2_be32(data_block + 0x00U) != JBD2_MAGIC ||
		jbd2_be32(data_block + 0x04U) != JBD2_COMMIT_BLOCK ||
		jbd2_be32(data_block + 0x08U) != sequence ||
		data_block[0x0CU] != JBD2_CHECKSUM_CRC32C || data_block[0x0DU] != 4U
	) {
		goto discard;
	}
	uint32_t expected_commit = jbd2_be32(data_block + 0x10U);
	if (jbd2_commit_checksum(journal, data_block) != expected_commit) goto checksum_fail;

	for (uint32_t index = 0U; index < recovered; index++) {
		if (!jbd2_write_fs_block(journal, journal->entries[index].home_block, journal->entries[index].data)) goto io_fail;
	}

	if (!block_device_flush(journal->device)) goto io_fail;
	journal->replay_count++;
	journal->replayed_block_count += recovered;
	journal->last_replay_sequence = sequence;
	journal->last_replay_blocks = recovered;
	goto finish;

	discard:
	journal->discard_count++;

finish:
	journal->sequence = sequence + 1U;
	if (journal->sequence == 0U) journal->sequence = 1U;
	journal->head = jbd2_next_log_block(journal, cursor);
	if (!jbd2_write_superblock(journal, 0U, journal->sequence, journal->head)) goto io_fail;
	if (!block_device_flush(journal->device)) goto io_fail;
	(void)kfree(data_block);
	(void)kfree(descriptor);
	return JBD2_STATUS_OK;

checksum_fail:
	(void)kfree(data_block);
	(void)kfree(descriptor);
	return JBD2_STATUS_CHECKSUM;

io_fail:
	(void)kfree(data_block);
	(void)kfree(descriptor);
	return JBD2_STATUS_IO_ERROR;
}
