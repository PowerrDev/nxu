# JBD2

## Overview

`vfs/jbd2.c` provides the write-ahead metadata journal used by writable ext4.
It is private storage infrastructure; VFS and vnode callers never issue JBD2
operations directly.

The journal is an internal ext4 file. The current mount path resolves journal
inode 8 before normal filesystem publication and requires its extent root to
map one contiguous physical range.

## On-disk byte order

JBD2 fields are big-endian even though ext4 metadata is little-endian.

The implementation therefore reads and writes journal fields with explicit
byte helpers rather than C structure casts.

## Journal state

`jbd2_t` owns:

```text
block device
filesystem block size
journal physical range
journal superblock image
transaction sequence/head
CRC32C seed
transaction entries
DMA-independent I/O scratch block
commit/replay counters
```

The transaction array contains final full-block metadata images indexed by
their ext4 home block. Staging the same home block twice replaces the existing
image.

## Supported format

NXU requires the version-2 journal superblock and checksum-v3 transaction
format with CRC32C.

The journal superblock UUID must match ext4's journal UUID.

Fast commits, asynchronous commit and checksum-v2 are rejected. The 64-bit
block-number capability is understood by checksum-v3 tags.

## Transaction record

One NXU transaction is encoded as:

```text
journal superVirtIOBlockFamily: active sequence/start
        |
descriptor block
        |
metadata image 0
metadata image 1
...
        |
commit block
```

The descriptor contains one checksum-v3 tag per metadata image. Tags record
the final ext4 home block and the CRC32C of the journal UUID, transaction
sequence and stored data block.

The first tag carries the journal UUID. Later tags use the same-UUID flag.

If a metadata image begins with the JBD2 magic word, the first word is escaped
in the journal copy and restored during replay.

## Descriptor checksum

The descriptor tail contains CRC32C over the journal UUID seed and complete
descriptor block with the checksum field cleared.

A descriptor with a mismatched tail is never replayed.

## Commit checksum

The commit record is the durability boundary.

Its checksum covers the journal UUID seed and complete commit block with the
checksum field cleared. A transaction is replayable only after a valid commit
record for the expected sequence is present.

## Commit ordering

`jbd2_commit()` performs synchronous write-ahead ordering:

```text
publish active journal superblock
flush

write descriptor
write metadata journal images
flush

write commit record
flush

checkpoint metadata home blocks
flush

mark journal checkpointed
flush
```

The commit record therefore cannot become durable before all journal metadata
images required by that transaction are durable.

## Recovery

At mount, `jbd2_recover()` examines the journal superblock.

For the pending sequence it:

1. validates the descriptor header and descriptor checksum;
2. parses checksum-v3 tags;
3. validates each journal metadata image;
4. validates the commit record;
5. restores escaped words;
6. checkpoints verified metadata to ext4 home blocks;
7. flushes the device;
8. advances the journal sequence/head and writes the journal superblock.

An incomplete transaction without a valid commit record is discarded.
Checksum failure aborts mount rather than replaying uncertain metadata.

## Immediate checkpoint policy

NXU currently checkpoints every successful commit immediately. It does not
keep a queue of committed-but-uncheckpointed transactions during normal
execution.

This gives recovery one intentionally constrained shape: at most one NXU
transaction can remain committed in the log after loss of execution between
the commit flush and the checkpoint flush.

The constraint keeps the first journal implementation small enough to audit
without weakening the write-ahead invariant.

## Crash injection

`jbd2_set_crash_after_commit()` arms a one-shot development hook.

When armed, `jbd2_commit()` returns after the commit block has been flushed and
before any metadata home block is checkpointed. The caller must halt instead
of continuing normal filesystem activity.

`make journal-crash` enables that path. `make journal-recover` boots the normal
kernel against the same disk and exercises replay.

## Locking

JBD2 does not own an independent scheduler lock. ext4 holds its per-mount lock
across transaction begin, staging and commit. A `jbd2_t` therefore has one
caller at a time.

## Limits

The current implementation supports at most 48 distinct metadata home blocks
per transaction and emits one descriptor block.

NXU does not yet emit or replay its own revocation blocks, batch multiple
committed transactions, perform asynchronous checkpointing, or reclaim log
space concurrently with writers.
