#ifndef NXU_VFS_JBD2_H
#define NXU_VFS_JBD2_H

#include <drivers/block/block_device.h>

#include <stdbool.h>
#include <stdint.h>

#define JBD2_TRANSACTION_MAX_BLOCKS 48U

typedef enum {
	JBD2_STATUS_OK = 0,
	JBD2_STATUS_INVALID,
	JBD2_STATUS_NOT_SUPPORTED,
	JBD2_STATUS_IO_ERROR,
	JBD2_STATUS_NO_MEMORY,
	JBD2_STATUS_NO_SPACE,
	JBD2_STATUS_CHECKSUM,
	JBD2_STATUS_CRASH_POINT
} jbd2_status_t;

typedef struct {
	uint64_t home_block;
	uint8_t *data;
} jbd2_transaction_entry_t;

typedef struct {
	block_device_t device;
	uint32_t block_size;
	uint64_t filesystem_blocks;
	uint64_t *journal_block_map;
	uint32_t journal_blocks;

	uint32_t first;
	uint32_t maximum;
	uint32_t sequence;
	uint32_t head;
	uint32_t feature_incompat;
	uint8_t uuid[16];
	uint32_t checksum_seed;

	uint8_t *superblock;
	uint8_t *io_block;
	uint8_t *transaction_storage;
	jbd2_transaction_entry_t entries[JBD2_TRANSACTION_MAX_BLOCKS];
	uint32_t entry_count;

	uint64_t commit_count;
	uint64_t replay_count;
	uint64_t replayed_block_count;
	uint64_t discard_count;
	uint32_t last_replay_sequence;
	uint32_t last_replay_blocks;

	bool active;
	bool transaction_active;
	bool checksum_v3;
	bool block64;
	bool crash_after_commit;
	bool crash_point_reached;
} jbd2_t;

/*
 * jbd2_init:
 *
 * Attach an ext4 journal block map and validate its version-2 superblock.
 * Journal logical blocks may be physically fragmented. NXU requires CRC32C
 * checksum-v3 transactions and rejects fast-commit and asynchronous-commit
 * journals.
 */
jbd2_status_t jbd2_init(
	jbd2_t *journal,
	block_device_t device,
	uint32_t block_size,
	uint64_t filesystem_blocks,
	const uint64_t *journal_block_map,
	uint32_t journal_blocks,
	const uint8_t expected_uuid[16]
);

void jbd2_destroy(jbd2_t *journal);

/*
 * jbd2_recover:
 *
 * Replay a committed transaction left in the log after an interrupted
 * checkpoint. Transactions without a valid commit record are discarded.
 */
jbd2_status_t jbd2_recover(jbd2_t *journal);

jbd2_status_t jbd2_begin(jbd2_t *journal);
void jbd2_abort(jbd2_t *journal);

/*
 * jbd2_stage:
 *
 * Replace the pending image of one filesystem metadata block. Repeated writes
 * to the same home block coalesce into one transaction entry.
 */
jbd2_status_t jbd2_stage(jbd2_t *journal, uint64_t home_block, const void *block);

/*
 * jbd2_overlay:
 *
 * Return the pending image for home_block when it is part of the active
 * transaction. The caller may otherwise read the block from its home address.
 */
bool jbd2_overlay(const jbd2_t *journal, uint64_t home_block, void *block);

/*
 * jbd2_commit:
 *
 * Commit the active transaction to the journal, force the commit record, then
 * checkpoint metadata to its home locations. The log is marked clean only
 * after checkpoint I/O has completed.
 */
jbd2_status_t jbd2_commit(jbd2_t *journal);

bool jbd2_is_transaction_active(const jbd2_t *journal);
void jbd2_set_crash_after_commit(jbd2_t *journal, bool enabled);
bool jbd2_crash_point_reached(const jbd2_t *journal);

#endif
