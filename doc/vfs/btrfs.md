# Btrfs (read-only)

A read-only Btrfs driver for arm64 and i386, structured so that write support,
copy-on-write transactions, snapshots and subvolume creation can be added
without reshaping it. It lives in `vfs/btrfs/`, mounts with
`vfs_mount("btrfs", device, path)`, and is tested against images made by the
real Linux stack (see [Fixtures](#fixtures-and-ground-truth)).

## Layering

```
   VFS (vfs/vfs.c, vnode.c)            vnode ops: lookup readdir read getattr readlink
        |                                      (create unlink write truncate -> READ_ONLY)
   btrfs_vfs.c   <- the ONLY file that knows vnodes and mounts (kernel only)
        |
   ---- pure core: no kernel headers; allocator, reader and log are injected ----
        |
   btrfs_replay   the log tree, read into memory and layered over dir/inode/file/csum lookups
   btrfs_file     EXTENT_DATA -> bytes: inline, regular, prealloc, holes, compressed extents,
                  data checksums
   btrfs_codec    zlib, LZO and ZSTD decoders (btrfs_zlib.c, btrfs_lzo.c, btrfs_zstd.c)
   btrfs_dir      DIR_ITEM lookup by crc32c name hash, DIR_INDEX cursor iteration
   btrfs_inode    INODE_ITEM, INODE_REF / INODE_EXTREF
   btrfs_root     ROOT_ITEM, subvolumes, "default", ROOT_REF / ROOT_BACKREF, subvolume points
   btrfs_tree     verified tree blocks, LRU cache, search, cross-leaf iteration (paths)
   btrfs_chunk    logical -> physical (sys_chunk_array, then the chunk tree); SINGLE, DUP, WIP: RAID
   btrfs_super    three superblock copies, validation, feature/device gating
   btrfs_csum     crc32c, xxh64; btrfs_hash: sha256, blake2b-256 (the four csum types)
   btrfs_io       reader interface: read N bytes at a byte offset
        |
   btrfs_io_block.c (kernel block_device_t)   btrfs_io_host.c (a file; host tests only)
```

A layer only calls downwards. `btrfs_format.h` holds every on-disk constant and
the parsed forms of every structure; all fields are read with explicit
little-endian byte accessors and **no byte buffer is ever cast to a struct**
(alignment and strict aliasing are safe on arm64 and i386). Every parser is
told how many bytes it may look at and refuses to read past them.

`btrfs_env_t` is the injected environment: `alloc` (zeroed), `release`, and
`log` (one finished line). The kernel glue plugs in `kcalloc`/`kfree`/`kputs`;
the host build plugs in a counting `malloc` so the tests can prove nothing
leaks. Log lines start with the emitting function's name (`BTRFS_LOG`) and use a
printf subset with no field widths, like `kprintf`.

## Key algorithms

**Superblock** (`btrfs_super.c`). Copies at 64 KiB, 64 MiB and 256 GiB, each
considered only if it fits on the device. A copy is valid when the magic
`_BHRfS_M`, its own byte number, its crc32c and its geometry (power-of-two
sector/node sizes, levels < 8, aligned roots) are right; the valid copy with
the highest generation wins, so a damaged primary falls back to a mirror. The superblock names its
own checksum type, which then covers every tree block and every data sector;
an unknown type number is corruption, not a feature.

**Chunk map** (`btrfs_chunk.c`). Built from `sys_chunk_array`, then completed
from the chunk tree. Sorted array, binary search, overlap and range checks
(everything must lie inside its device). `btrfs_map_logical` turns a logical
address into the copies of the piece it falls in, each a (device id, physical
offset) pair, and `btrfs_read_logical` reads across pieces and devices.
**Multi-device and RAID support is work in progress (WIP)**: it is committed
separately (`WIP vfs/btrfs: ...`, easy to revert), it reads every profile below
byte-identically to Linux on the host and in both kernels, but it has had far
less testing than the single-device paths (no write, no degraded mode, no
parity reconstruction, no fuzzing of multi-device images), so treat it as
experimental:

| Profile | Mapping |
|---|---|
| SINGLE | 1 stripe |
| DUP, RAID1, RAID1C3, RAID1C4 | every stripe is a whole copy (DUP: two on one device; the others on different devices): 2, 2, 3, 4 copies |
| RAID0 | `stripe_len` bytes on one device, then the next, round robin; 1 copy |
| RAID10 | as RAID0 over pairs (`sub_stripes` = 2), each stripe mirrored: 2 copies |
| RAID5, RAID6 | data stripes rotate with the row (`(column + row) mod n`), parity takes the rest; a read goes straight to the data stripe: 1 copy, **no reconstruction from parity** |

A block or data extent that fails verification on one copy is re-read from the
next copy in stripe order (`mirror_fallbacks` counts each failed copy that had a
successor) and is served only if a later copy verifies; if none does, the read
fails. Chunk geometry is validated per profile (stripe count, `stripe_len` a
power of two, chunk length a multiple of a full stripe row, every stripe inside
its device).

**Devices** (`btrfs_fs_open_devices`, WIP). The caller supplies all devices of the
filesystem in any order (`btrfs_fs_open` is the one-device case; the kernel takes
up to 8, the virtio-blk driver 4 in total). Every device's superblock is read and
they must agree on the fsid and have distinct device ids; the highest generation
wins (a device that lags is used but logged), each device must be as large as its
own device item says, and the chunk tree's DEV_ITEMs are matched with the
supplied devices by id **and uuid**. Every device the superblock lists
(`num_devices`) must be present: a missing one is refused with
`BTRFS_ERR_MISSING_DEVICE`, never mounted degraded (degraded reads would serve
holes or fall back on parity this driver does not use). More devices than the
filesystem has, a device of another filesystem, or two devices with one id are
`BTRFS_ERR_INVALID`.

**Tree blocks** (`btrfs_tree.c`). A block is accepted only if its crc32c, own
address, fsid (metadata_uuid when set), level, generation (equal to what the
parent recorded, never newer than the superblock), owner, item layout (item
data tiles the block end to end and never reaches into the headers; keys
strictly ascend; child pointers sector aligned) and first key (equal to the
parent's key) are right. Blocks live in an LRU cache keyed by logical address
(12 to 48 blocks, about 512 KiB), pinned while a path holds them. A search
(`btrfs_tree_search*`) descends by binary search and leaves a
`btrfs_path_t`; `btrfs_path_next/prev` step across leaves through the parent
slots. Since child level is exactly parent level - 1 and iteration only moves
to strictly larger (or smaller) keys, a corrupt image cannot loop or recurse
without bound; no recursion exists in the core.

**Subvolumes** (`btrfs_root.c`). `ROOT_ITEM` lookup takes the last item with the
objectid; the default subvolume is the `default` DIR_ITEM of the root tree;
`ROOT_REF`/`ROOT_BACKREF` give the tree of subvolumes. A directory entry whose
location is a `ROOT_ITEM` is a subvolume point: the walk continues in that
subvolume at its root inode. A point with no live subvolume behind it (a
snapshot copies the entries of nested subvolumes but not the subvolumes) is an
empty directory, as Linux shows it. Mount option: `subvol_id`.

**Directories** (`btrfs_dir.c`). Lookup by `crc32c(~1, name)` into DIR_ITEM
(hash collisions pack several names into one item; all are compared);
enumeration by DIR_INDEX with a cursor that is "the next index to look at", so a
saved cursor resumes exactly after the entry it came with. The VFS readdir
offset is 0 for `.`, 1 for `..`, then that cursor.

**File data** (`btrfs_file.c`). Position on the last EXTENT_DATA at or before
the offset and walk forward: inline, regular (reading the slice
`[offset, offset+num_bytes)` of a possibly larger disk extent), preallocated
(zeros), explicit hole (zeros), implicit gap (zeros), tail up to `i_size`
(zeros). Every iteration advances the position or moves to a strictly later
item. Data checksums are verified on every read (see
[Bad extents](#bad-extents-and-data-checksums)); compressed extents are decoded
(see [Compression](#compression)).

### Log tree replay

`fsync()` does not commit a transaction. It writes what changed into a log tree
and points `super.log_root` at it; if the machine stops before the next commit,
Linux replays the log at the next mount. This driver never writes, so instead of
replaying into the trees it reads the log into memory in `btrfs_fs_open()`
(`btrfs_replay.c`: the log root tree, one log tree per subvolume, every item
copied into one pool, capped at 16 MiB and 256 Ki items) and layers it over the
committed trees, so a volume that Linux would replay shows the same thing here:

- **Inodes**: a logged INODE_ITEM replaces the committed item; inodes that exist
  only in the log are found. A directory the log changed keeps its committed
  size plus 2 x name length for every entry the replay links and minus that for
  every entry it unlinks (Linux's rule; a new directory starts from the logged
  size).
- **Directory entries**: DIR_INDEX items of the log and the names of logged
  INODE_REF items (Linux replays those as links) add or replace entries by index
  (the log wins an index it shares); entries in a logged range (`DIR_LOG_INDEX`
  `[first, last]`) that the log does not repeat and names that a logged INODE_REF
  no longer lists (rename, unlink) are removed. A directory the log touched is
  looked up by walking the merged listing (the hash items exist only in the
  committed tree).
- **File extents**: logged EXTENT_DATA items replace what they overlap. A file
  with logged extents is read through a merged extent list (committed extents
  trimmed around the logged ones, so a tail keeps its offset into the disk
  extent; holes punched by the log are explicit hole extents), which also covers
  truncation, appends, overwrites inside an extent and compressed extents.
- **Checksums**: the EXTENT_CSUM items of the log verify the logged data (they
  are consulted before the csum tree), so replayed data is verified like any
  other.
- **INODE_REF enumeration** (hard-link counts): a logged item replaces the
  committed item of the same (inode, parent).

`ignore_log_tree` skips all of this (the state of the last commit). Not
understood, so the mount is refused with `log-tree` rather than showing
something wrong: item types the log tree of a subvolume should not hold for this
driver (extended inode refs, `DIR_ITEM`, orphan items, root items in a subvolume
log, entries that lead into another subvolume), a log with more than 32 subvolume
trees, an inode that is referenced but exists neither in the log nor in the
tree, and a logged extent that would have to cut the front off an inline extent.
XATTR items and `DIR_LOG_ITEM` (the name-hash range) are ignored: this driver
does not interpret xattrs and looks names up by walking. Inodes that the log
unlinks completely disappear because their directory entries do (Linux removes
the orphan later; nothing here can show it). The device is never written.

### Compression

`btrfs_fs_open()` registers the three decoders in `fs->decompressors[]`; a
method without a decoder (only a damaged image can name one) fails the read with
`BTRFS_ERR_UNSUPPORTED_COMPRESSION`, never with garbage. The decoders are
freestanding (`btrfs_codec.h`) and share one contract: they get the on-disk
extent (`disk_num_bytes`, possibly with zero sector padding) and a buffer of
exactly `ram_bytes`, write at most that many bytes, and return `BTRFS_OK` only
for a complete, well-formed stream; a stream that decodes to fewer bytes is
zero filled (Linux does the same for the rounded-up last extent), anything else
is `BTRFS_ERR_CORRUPT`. `ram_bytes` above 128 KiB (what Btrfs writes) is refused
before any decoder runs, so a decompression bomb cannot grow past the extent's
stated size.

- **zlib** (`btrfs_zlib.c`): RFC 1950/1951 inflate in the style of puff.c; the
  output is the window, no allocation; Adler-32 verified.
- **LZO** (`btrfs_lzo.c`): LZO1X with `lzo1x_decompress_safe` semantics inside
  Btrfs' framing (total length, then per-sector segments, headers never
  straddling a sector, every segment its own stream, so distances are checked
  against the segment).
- **ZSTD** (`btrfs_zstd.c`): RFC 8878 frames: raw/RLE/compressed blocks, raw/RLE/
  Huffman literals (1 and 4 streams, direct and FSE weights, treeless), all four
  sequence table modes, repeat offsets, content checksum verified. No
  dictionaries, windows above 128 KiB refused. Work memory (about 11 KiB of
  tables plus one block of literals) is one allocation through the injected
  `btrfs_env_t`.

Reads: the compressed bytes are read through the normal data path (so they are
verified against the csum tree, which covers what is on disk, and DUP falls
back), decoded, and the requested slice of the decoded extent is copied out;
partial-extent reads (`offset` inside the decoded extent, several files or
ranges referring to different parts of one extent) fall out of that. Inline
compressed extents decode from the item. **Memory:** decoded data lives in
`fs->extent_cache`, one buffer of at most 128 KiB allocated on first use and
freed at close, holding the last decoded extent, so reading a file in small
steps decodes each extent once (`extents_decoded`, `extent_cache_hits` in the
stats). While a decode runs there is also the compressed copy (at most 128 KiB)
and the decoder's work memory; both are freed before the read returns. Peak is
therefore about 300 KiB per mounted filesystem, and a failed decode leaves the
cache empty.

### Bad extents and data checksums

Every sector of a regular extent is compared with the csum tree before its
bytes reach the caller; `noverify` at mount is the only way out (metadata is
always checked). The policy, in `btrfs_read_data_verified()`:

- The range is widened to whole sectors and read in 64 KiB pieces; a piece is
  copied out only after it verified, so a failing read never hands out
  unverified bytes from that piece.
- A piece must verify on some copy of its chunk. DUP (and later mirrored)
  chunks are tried first copy first, then the next; each failed copy that has a
  successor counts in `stats.mirror_fallbacks`, so a damaged first copy
  self-heals for the reader (the device is never written).
- If no copy verifies the whole read fails with `BTRFS_ERR_CSUM` (VFS: I/O
  error) and no further piece is read. A read that stays clear of the bad sector
  still works: verification is per sector, not per extent. `stats.csum_failures`
  and `stats.data_bad_reads` count it, the log says which logical address.
- No data checksum, no verification: preallocated extents and holes have no
  data and no csum items; inodes with the NODATASUM flag (mount option
  `nodatasum`, chattr +C) are read as they are.
- A sector of a file that must carry checksums but has no csum item is treated
  as a failure too (`stats.data_csum_missing`), because serving it would be
  serving unverified data. (Linux serves it; the strict reading is deliberate.)
- The csum tree is searched through a cursor that keeps the current
  EXTENT_CSUM item pinned, so a run of extents needs one tree search per item,
  and the leaves come from the tree-block cache.

## Feature matrix

| Area | Status |
|---|---|
| crc32c checksums | supported (table driven, in the core) |
| xxhash64, sha256, blake2b checksums | supported (tree blocks, superblock, data) |
| SINGLE, DUP chunks (metadata and data) | supported |
| RAID0, RAID1, RAID10, RAID1C3, RAID1C4, SINGLE across devices (metadata and data) | **WIP**, verified against Linux: every device supplied (up to 8 in the core, 4 boot disks in the kernel), any order |
| RAID5, RAID6 (all devices present) | **WIP**: plain reads only; no parity reconstruction, so a checksum failure is an error |
| missing device (degraded mount), device of another filesystem, duplicate device id, device uuid differing from the chunk tree | refused: `a device of the filesystem is missing` / `invalid argument` (was `unsupported RAID or multi-device profile` before the WIP) |
| Superblock mirrors, highest valid generation | supported |
| mixed block groups, skinny/no skinny metadata, no-holes and explicit holes, free-space-tree, v1 space cache, block-group-tree, extended irefs, squota, metadata_uuid, big metadata | supported (only how metadata is written or accounted for changes) |
| zoned, extent-tree-v2, raid-stripe-tree, unknown incompat bits | refused: `unsupported feature` |
| unreplayed log tree (fsynced, not yet committed) | replayed in memory over the trees (see [Log tree](#log-tree-replay)); `ignore_log_tree` shows the last commit instead; a log this layer does not understand is refused (`log-tree`) |
| Subvolumes, snapshots, read-only snapshots, nested subvolumes, default subvolume, mount by id | supported, read-only |
| Inline, regular, prealloc, hole, partial-extent reads | supported |
| Compressed extents (zlib, lzo, zstd; any level; inline, partial references, sparse) | supported |
| Encrypted extents, other encodings | refused per extent |
| Data checksums | verified on every read by default; `noverify` opts out (`verify_data` spells the default); NODATASUM files, holes and prealloc are exempt |
| Directories with thousands of entries, 255-byte names, unicode names, hard links (INODE_REF and EXTREF), symlinks (inline target up to 4095 bytes), fifo, socket, char and block devices | supported |
| xattr items | skipped safely |
| ACLs, quotas, send/receive, reflink-aware reads beyond plain extents | not interpreted (nothing is needed to read the bytes) |
| Any mutation | `VFS_STATUS_READ_ONLY` |

Sector sizes above the page size and nodesizes 4 KiB to 64 KiB are fine
(`s16k`, `n4k`, `n64k` fixtures).

## VFS integration

`btrfs_register()` publishes the filesystem type; it does no I/O. The kernel
registers it **only from the boot-argument test hooks** (arm64
`kern/kern_init.c`, i386 `kern/i386/userland_init.c`), never at boot, so the
arm64 `vfs_dump` line `filesystems: N` and the whole boot log are unchanged. To
make Btrfs available in a normal boot, add `btrfs_register()` next to
`ext4_register()` in the two boot paths (this changes that one log line).

Options for the next mount go through `btrfs_set_next_mount_options()`
(`subvol_id`, `verify_data`, `noverify`, `ignore_log_tree`, `extra_devices`) because `vfs_mount` has no
options argument; `btrfs_last_mount_status()` keeps the precise cause of a
failed mount, since VFS statuses are coarse (`UNSUPPORTED_*` map to
`NOT_SUPPORTED`, magic to `INVALID`, corruption/checksum/truncation to
`IO_ERROR`).

Shared-header changes (additive): `vnode_operations_t` gained optional
`getattr` and `readlink`, `vnode_attr_t` was added, and `VNODE_TYPE_SYMLINK`,
`_FIFO`, `_SOCKET` were appended to `vnode_type_t`. Filesystems that do not set
the new operations answer `NOT_SUPPORTED`.

Vnodes are resident until unmount (like ext4): one per (subvolume, inode), found
through a hash. `vfs_lookup` caps a path component at 63 bytes
(`VFS_NAME_MAX`), so a 255-byte name is reachable through `vnode_lookup` and
`readdir`, not through a path.

`btrfs_dump()` prints, per mount, the superblock facts, chunk count, cache
counters and I/O counters.

## Tests

```
make test-btrfs-host BUILD_ROOT=<scratch>        # native, ASan + UBSan, 294 checks
make test-i386-btrfs BUILD_ROOT=<scratch>        # in-kernel, second..fourth virtio-blk-pci, 50 checks
make test-arm64-btrfs BUILD_ROOT=<scratch> CONFIG=base   # in-kernel, virtio-blk-device, 50 checks
```

Always pass a scratch `BUILD_ROOT`. None of them touches `disk.img` or
`tools/DiskRoot` (the arm64 test copies `disk.img`); if `disk.img` looks stale to
make, pass `DISK=`, `DISK_ROOT=` and `DISK_FORMAT_STAMP=` pointing at scratch copies
so the rebuild does not dirty the tracked files. Counts above are from the last run
of the commands as written (2026-09-20); the sweep length is `BTRFS_SWEEP_ITERS`
(default 300) times `BTRFS_SWEEP_SEEDS` (default 2).

**Host** (`tools/btrfs/test_host.sh`, tool `tools/btrfs/host/btrfs_host.c`):
superblock facts against `btrfs inspect-internal dump-super`; a full walk of
each of 18 fixtures compared with the manifest Linux wrote (every path, type,
mode, owner, size, nlink, inode, rdev, mtime, symlink target, crc32c and
sha256 of every file), with and without data checksum verification; partial
reads at random offsets, across and past EOF and in odd steps, all checked
against the full read; readdir cursor resume; lookup of every name by hash and
of a missing name; hard-link counts against INODE_REF/EXTREF; every subvolume
mounted by id; subvolume list against `btrfs subvolume list`; the default
subvolume; compressed extents of every algorithm and shape (`mix-*`, `comp-*`:
zlib 1/9, LZO, ZSTD 1/3/15, the 128 KiB extent boundary, sparse files, extents
only partly referenced, incompressible data) and all four checksum types, each
walked against Linux; the decoders alone (`tools/btrfs/test_codec.sh`: streams from
python zlib and the zstd command and an LZO generator, output cap, truncation,
bombs, mutation sweeps under ASan/UBSan, sha256/blake2b against hashlib);
multi-device filesystems (`md-*`, real `mkfs.btrfs` over 2 to 4 devices: RAID1, RAID0,
RAID10, RAID1C3, RAID1C4, RAID5, RAID6, SINGLE data with DUP metadata; every
profile walked against Linux, also with the devices in reverse order; missing,
foreign and duplicate devices refused; a damaged copy falling back to another
device, all-but-one copies damaged, every copy damaged, RAID0 (one copy) failing);
refusal of what is unsupported; named
corruptions (`tools/btrfs/corrupt.py`: superblock magic/checksum/features/log
root/geometry, truncation at several sizes, root/chunk/subvolume tree blocks,
DUP self-healing); the data-checksum policy (a damaged data extent fails a
read of it, DUP heals from the second copy, both copies bad fails, `noverify`
and NODATASUM serve the bytes, a missing csum item refuses, verification
cost stays bounded); and a seeded corruption sweep (bit flips, zeroing,
scribbles, truncation, checksummed-but-lying superblocks and tree blocks,
injected read errors, injected allocation failures) which may only produce
errors or the good image's results, never a crash, hang (read budget and
alarm), out-of-bounds access, leak or different data reported as success.

**In-kernel** (`vfs/btrfs/btrfs_selftest.c`, both architectures): boot argument
`btrfs-test=<spec>[,<spec>...]`, the Nth spec against the Nth block device
after the root disk (the virtio-blk driver takes four devices in total, so
three fixture devices per boot; a spec ending in `+devN` takes N consecutive
devices for one multi-device filesystem, so the 4-device profiles run on the
host only). `NAME[@SUBVOLID][+verify|+noverify]` mounts, walks through the
public VFS interface against the expected listing generated from Linux's
manifests (`btrfs_selftest_data.h`, `tools/btrfs/gen_selftest_data.py`), checks
readdir resume, odd-offset and EOF reads, that every mutating operation
returns `READ_ONLY` and the device write counter stays put, and unmounts (which
also proves no vnode reference leaked). `!STATUS` requires a clean mount
failure with exactly that status and a good mount afterwards. The host script
also compares each fixture disk's sha256 before and after the boot. arm64
notes: QEMU virt hands out virtio-mmio slots top-down while the kernel probes
bottom-up, so `test_arm64.sh` defines the fixtures in reverse and the root disk
last; the kernel also needs the gpu/keyboard/mouse devices of the normal boot
command, and the sound device is defined after everything else so that it takes
the lowest slot and leaves the block order alone. The arm64 test ends with a PSCI `SYSTEM_OFF`.

## Booting a Btrfs disk and looking at it

`make run-i386-btrfs` boots the i386 kernel with a Btrfs image as its only disk,
mounts it read-only, prints the tree (and/or one file) to the serial console and
exits. Unlike `btrfs-test=`, which grades the fixtures against expected
listings, this works on any image (`vfs/btrfs/btrfs_list.{c,h}`, arch-neutral).

    make run-i386-btrfs BTRFS_FIXTURE=tree            # a committed fixture
    make run-i386-btrfs BTRFS_IMAGE=/path/disk.img    # any Btrfs image; never modified (QEMU snapshot=on)
    make run-i386-btrfs BTRFS_FIXTURE=minimal BTRFS_CAT=/small.txt BTRFS_LS=0

Options: `BTRFS_CAT=/path` prints that file (first 8 KiB, non-printable bytes as
`.`), `BTRFS_LS=0` skips the tree, `BTRFS_SUBVOL=<id>` mounts that subvolume,
`BTRFS_NOVERIFY=1` skips data checksums, `BTRFS_MAX=<n>` bounds the printed
entries (default 512; the summary still counts everything), `BTRFS_TIMEOUT=<s>`.
The listing is `<type><rwx mode> <nlink> <size> <path>[ -> target]`. The script
is `tools/btrfs/run_i386.sh`; the kernel side is the boot arguments `btrfs-ls`,
`btrfs-cat=<path>`, `btrfs-dev=<n>`, `btrfs-subvol=<id>`, `btrfs-noverify=1` and
`btrfs-max=<n>`, handled in `kern/i386/userland_init.c` before the ext4 root is
mounted, so no root disk is needed. arm64 has no equivalent yet. A non-Btrfs or
damaged image fails the mount cleanly and the run exits nonzero.

## Fixtures and ground truth

See `tools/btrfs/fixtures/README.md`. Images and manifests come from an official
Alpine Linux 3.24.2 guest (btrfs-progs 6.17.1, kernel 6.18.52) run under QEMU by
`tools/btrfs/make_fixtures.sh`, so the driver is graded against Linux, not
against itself.

## Write-support extension points

Nothing below is implemented; these are the seams and what already carries the
needed information.

- **Transaction / COW layer.** Insert between `btrfs_tree` and the layers above.
  Reads go through `btrfs_block_get()` and `btrfs_path_t`, so a modifying layer
  can hand out shadowed blocks from the same cache: `btrfs_block_t` already has
  a `dirty` flag, its `generation` and `owner`, and the cache pins blocks by
  reference count. A commit updates the root pointers held in `btrfs_tree_t`
  (`bytenr`, `generation`, `level`), which every search takes as an argument,
  then writes the new root tree and the superblock (all copies, highest
  generation last) through a writer that mirrors `btrfs_reader_t`.
- **Extent allocator.** Needs the extent tree and the block-group/free-space
  information, which the driver already tolerates but does not read. The chunk
  map (`btrfs_chunk_map_t`) already records type and profile per chunk and is
  where new chunks would be added; `btrfs_map_logical` returns every stripe so
  writes can fan out to DUP copies.
- **Tree modification.** Insert/delete/split/merge belong beside
  `btrfs_tree_search` and reuse its path (one pinned block and one slot per
  level). Item accessors already validate offsets, and leaf verification
  documents the packing invariants that a writer must maintain.
- **Checksums.** `btrfs_csum_crc32c` is the single place a block checksum is
  produced; the csum tree is opened on demand by the data path and would gain
  insert/delete for new extents.
- **Snapshots and subvolume creation.** A snapshot is a new ROOT_ITEM (with the
  same `bytenr` and a bumped reference count) plus a ROOT_REF/ROOT_BACKREF pair
  and a DIR_ITEM/DIR_INDEX in the parent; `btrfs_subvol_t` carries the
  root item (generation, uuid, parent_uuid, flags), and `btrfs_root_*` already
  reads every one of these items. Because a subvolume tree's blocks are
  accepted for any fs-tree owner, blocks shared between a subvolume and its
  snapshots already verify.
- **Directory and inode changes.** `btrfs_dir` already models the paired
  DIR_ITEM/DIR_INDEX entries and next-index allocation is "one past the last
  DIR_INDEX"; INODE_REF/EXTREF handling exists for hard links.
- **VFS.** The mutating vnode operations exist and return `READ_ONLY`; they are
  the entry points a writable mount would fill in, together with the mount's
  sync/unmount.
- **Log tree.** Read-only replay exists (below); a writer has to do it for real
  (apply the log into the trees in a transaction and clear `log_root`) before it
  writes anything.

## Not done / follow-ups

- Multi-device and RAID (WIP, see above): finish and review it (sweeps over the
  multi-device fixtures, a kernel mount API that takes more than three extra
  devices, device scanning by fsid instead of the caller listing the devices),
  RAID5/6 parity reconstruction (a checksum failure or a missing device on
  RAID5/6 is an error today), and degraded mounts in general.
- Not verified: multi-device images under the corruption sweeps (only the
  single-device fixtures are swept); a multi-device set larger than three extra
  devices in a kernel (the virtio-blk driver has four devices in all, so RAID10,
  RAID1C4 and RAID6 run on the host only); real hardware (everything runs under
  QEMU).
- Write support (above).

## References

- Linux `include/uapi/linux/btrfs_tree.h` and `btrfs.h` (constants, structures)
- btrfs.readthedocs.io: On-disk format, Trees, Subvolumes, Btree items pages
- btrfs-progs 6.17.1 (`mkfs.btrfs`, `btrfs inspect-internal dump-super/dump-tree`)
- Alpine Linux 3.24.2 netboot (dl-cdn.alpinelinux.org), used only to build fixtures
