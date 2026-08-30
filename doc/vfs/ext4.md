# ext4

## Overview

NXU mounts a writable ext4 filesystem at `/disk` through the ordinary VFS
mount and vnode interfaces. The filesystem consumes `block_device_t`; no ext4
code depends on VirtIO transport details.

The development configuration enables metadata checksums and an internal JBD2
journal. Metadata mutation is transactional. Regular file data is written to
its home blocks before the metadata transaction is committed, providing the
ordered-data policy used by this implementation.

```text
VFS
 |
 vnode
 |
ext4
 |\
 | +-- CRC32C metadata validation
 |
 +---- JBD2 metadata transaction
          |
     block_device_t
          |
     VirtIO Block
```

## Mount requirements

A writable mount requires:

- extents;
- an internal journal in inode 8;
- `metadata_csum` with CRC32C;
- initialized inode and block metadata;
- a journal using checksum-v3 records;
- a journal whose inode resolves to one contiguous physical range.

The development profile rejects layouts whose mutation or recovery rules are
not implemented, including `meta_bg`, `bigalloc`, inline data, encryption,
casefold, orphan-file state and fast commits. Directory indexing remains
disabled for the writable development image.

Unknown incompatible features fail the mount before the root vnode is
published.

## Metadata checksum seed

The filesystem UUID establishes the CRC32C checksum seed unless the explicit
checksum-seed feature supplies `s_checksum_seed`.

The primary superblock is special: its checksum is calculated directly over
the superblock bytes preceding `s_checksum`.

Metadata is never trusted merely because the block device returned data.
Checksum verification occurs before allocator, inode, extent or directory code
consumes mutable structures.

## Group descriptors

Each group descriptor is verified before use. With `metadata_csum`, the group
number and descriptor contents are incorporated into CRC32C and the low 16
bits are stored in `bg_checksum`.

A group descriptor owns:

```text
block bitmap location
inode bitmap location
inode table location
free block count
free inode count
used directory count
bitmap checksums
metadata flags
```

NXU accepts 32-byte and 64-byte descriptor layouts. The lesson image uses
the 64-byte form.

## Allocation bitmaps

Block and inode bitmap checksums are validated whenever allocator state is
read. A 64-byte descriptor stores the complete 32-bit bitmap checksum across
its low and high checksum fields.

Mutation order within a transaction is:

```text
read + verify bitmap
        |
change allocation bit
        |
refresh bitmap checksum
        |
refresh group counters
        |
refresh group checksum
        |
stage bitmap + descriptor
```

The superblock free-space counters are staged in the same transaction.

## Inodes

Inode checksums cover:

```text
checksum seed
inode number
inode generation
complete inode image
```

The checksum fields themselves are zero while calculating the value. Inodes
with sufficient extra inode space store both halves of the checksum; older
layouts retain the low half only.

Every inode read validates its checksum before exposing fields to vnode code.
Every inode-table write regenerates the checksum after all mutable fields have
been encoded.

## Extents

Read-side extent traversal retains support for trees up to the ext4 depth
limit used by this driver. External extent blocks are checksum-validated using
the owning inode number and generation.

Write-side mutation remains intentionally restricted to a depth-zero extent
root resident in `i_block`. Up to four discontiguous extents can therefore be
represented directly; adjacent logical and physical blocks are merged.

A request which would require extent-tree node allocation fails instead of
emitting a partially understood tree.

## Directories

The writable profile uses linear directories.

Every directory data block reserves an ext4 directory checksum tail. Before
lookup or mutation, NXU verifies the tail and CRC32C calculated from the
filesystem checksum seed, directory inode number, inode generation and valid
directory bytes.

Insertion, removal and new-directory initialization regenerate the checksum
before the metadata block is staged in the journal.

## Metadata transactions

All metadata mutation begins with `ext4_transaction_begin_locked()`.

The filesystem first makes the ext4 `RECOVER` incompatibility flag durable.
After that point, modified metadata blocks are staged in JBD2 rather than
written directly to their home locations.

A typical file creation transaction contains some subset of:

```text
inode bitmap
block bitmap
group descriptor
inode-table block
parent directory block
superblock
```

Repeated changes to the same metadata block coalesce into one final transaction
image.

When JBD2 reports a completed checkpoint, ext4 clears the recovery flag and
flushes the updated superblock.

## Ordered file data

Regular file contents are not journaled.

For writes that allocate or modify file data, NXU writes data blocks to their
home addresses before committing the metadata that makes those blocks visible.
The metadata transaction then records inode size, extents, allocation bitmaps
and counters.

This prevents a committed extent from exposing data that had not yet reached
the block device in the normal write path.

## Journal recovery

Mount initializes the internal JBD2 journal before publishing inode 2.

If the journal contains a committed transaction that was not checkpointed,
JBD2 verifies its descriptor, tag data and commit checksums, writes the final
metadata images to their home blocks, flushes the device, and advances the
journal superblock. ext4 then rereads and verifies its primary superblock and
refreshes in-memory free-space counters.

No vnode is externally reachable until recovery succeeds.

Transactions without a valid commit record are discarded by the current
recovery path.

## Crash test

The build provides a deterministic recovery test:

```text
make journal-crash
```

The special kernel creates a stable empty file, arms the JBD2 crash point and
writes one message. JBD2 flushes the descriptor, metadata images and commit
record, then deliberately stops before home metadata is checkpointed.

The kernel prints that the crash point has been reached and halts.

Close QEMU and boot the ordinary kernel:

```text
make journal-recover
```

Recovery must replay the committed transaction. The ordinary ext4 self-test
then reads `/disk/NXU/journal-recovery.txt` and validates its contents.

The crash build never intentionally corrupts the journal; it models loss of
execution between commit and checkpoint.

The crash test also verifies that the write reached the dedicated post-commit
crash point rather than accepting an unrelated I/O failure as success.

During replay NXU rejects unknown tag flags, a first tag that incorrectly
claims `SAME_UUID`, and any tag that targets a block belonging to the journal
itself. A successful replay records the recovered transaction sequence and
metadata-block count for `ext4_dump`.

After closing the recovery boot, validate the resulting image independently:

```text
make disk-check
```

The expected result is a complete five-pass `e2fsck` run with no repair
questions or filesystem warnings.

## Sync and clean unmount

The final filesystem lifecycle path exposes explicit synchronization through
VFS. Since normal NXU JBD2 commits are synchronously checkpointed, ext4 sync
does not need to invent another transaction. It verifies that no transaction is
active, rejects a mount that still requires recovery, and issues a block-device
flush so volatile transport caches are committed.

Clean unmount runs the same sync step and then verifies vnode ownership. Cached
non-root vnodes may have only their filesystem residency reference; the root may
also have its mount reference. Additional references mean an open file or active
lookup still owns the vnode, so teardown returns `VFS_STATUS_BUSY`. Once clean,
vnodes are deactivated and all mount-private JBD2 and ext4 storage is released.

## Locking

Each ext4 mount has one spin lock.

The mount lock serializes:

- inode and extent mutation;
- allocation bitmaps;
- free-space counters;
- directory mutation;
- the single active JBD2 transaction;
- resident vnode-cache updates tied to metadata changes.

The current single-CPU kernel does not issue concurrent block requests for one
ext4 mount while this lock is held.

## Invariants

The writable implementation maintains these rules:

- unverified metadata is never used for mutation;
- every metadata image written by NXU carries a refreshed checksum;
- one mount has at most one active JBD2 transaction;
- a metadata home block is not checkpointed before its commit record is
  durable;
- the ext4 recovery flag is durable before journal metadata is published;
- normal transaction completion clears the journal before clearing ext4
  recovery state;
- VFS never depends on ext4 on-disk structures.

## Current limits

The filesystem is intentionally narrower than Linux ext4.

Not yet implemented:

- external extent-tree allocation or rebalancing;
- indexed-directory mutation;
- xattrs and ACLs;
- symlinks and hard-link creation;
- permissions and ownership enforcement;
- timestamps;
- orphan-file processing;
- fast commits;
- journal revocation records emitted by NXU;
- multiple simultaneously outstanding JBD2 transactions;
- journal batching or asynchronous checkpointing.

The JBD2 writer immediately checkpoints every normal committed transaction.
Consequently, the recovery implementation only needs to retain one pending
NXU transaction after an interrupted checkpoint. This is a deliberate
foundation constraint, not a claim of complete JBD2 compatibility.
