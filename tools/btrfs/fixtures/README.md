# Btrfs fixtures

Small Btrfs images made with the **real** Linux Btrfs stack, plus the ground
truth Linux reported for them. They are what the host tests
(`make test-btrfs-host`) and the in-kernel tests (`make test-i386-btrfs`,
`make test-arm64-btrfs`) run the driver against.

## How they are made

`tools/btrfs/make_fixtures.sh` boots the official Alpine Linux netboot under
QEMU (`qemu-system-x86_64`, hvf when available), installs btrfs-progs with
`apk`, and runs `tools/btrfs/guest/build_fixtures.sh` in the guest:

| Component | Version |
|---|---|
| Alpine Linux | 3.24.2 (x86_64 netboot, `vmlinuz-virt`, `initramfs-virt`, `modloop-virt`, sha256 pinned in the script) |
| Linux kernel in the guest | 6.18.52-0-virt |
| btrfs-progs | 6.17.1 (`mkfs.btrfs`, `btrfs`) |
| python3 in the guest | 3.14.7 |

Downloads are cached in `$HOME/.cache/nxu-btrfs-lab` (never in the repository).
Each image is formatted with `mkfs.btrfs`, populated through the Linux driver
(`mount -o loop`, `tools/btrfs/guest/pop.py`), checked with
`btrfs check --readonly`, and compressed with `zstd -19`. The one exception is
`s16k`: x86 Linux cannot mount a 16 KiB sector size, so it is populated with
`mkfs.btrfs --rootdir`.

To regenerate: `tools/btrfs/make_fixtures.sh` (about 6 minutes), or
`--only <group>` for one of `basic tree deep subvols variants compressed refusal`.
`tools/btrfs/make_fixtures.sh --smoke` only prints what the lab has. After
regenerating, run `python3 tools/btrfs/gen_selftest_data.py >
vfs/btrfs/btrfs_selftest_data.h` so the in-kernel expected listings follow.
Contents are deterministic; fsids derive from the fixture name. Inode
timestamps are whenever the images were built.

## Files per fixture

| File | Meaning |
|---|---|
| `NAME.img.zst` | the image (128 MiB unless noted, mostly zeros) |
| `NAME.manifest` | Linux's view of the mounted filesystem: tab-separated path, type, mode, uid, gid, size, nlink, inode, rdev, mtime, crc32c, sha256, symlink target, flag (`compressed`) |
| `NAME.info` | `btrfs inspect-internal dump-super -f` output and the level of the fs tree |
| `NAME.subvolN.manifest`, `NAME.subvols` | (subvols fixtures) one manifest per subvolume id mounted with `subvolid=N`, and `btrfs subvolume list` |
| `NAME.default` | (subvols-default) the id given to `btrfs subvolume set-default` |

## The set

| Fixture | What it is for |
|---|---|
| `empty`, `minimal` | empty root; an inline file and an empty file |
| `tree` | nested dirs, 2500 entries in one directory (multi-leaf DIR_ITEM/DIR_INDEX), name-hash collisions, 255-byte and unicode names, hard links, short/long (2749, 4000 byte) symlinks, fifo/char/block/socket, permissions, xattrs, sparse, 48-extent file written out of order, overwritten file (extents referencing part of a bigger one) with a punched hole, PREALLOC files, nodatacow, grown and truncated files |
| `deep` | 4 KiB nodes, fs tree level 2, 400 hard links from one directory (INODE_EXTREF), 9000 small files |
| `subvols`, `subvols-default` | subvolume, nested subvolume, snapshot, read-only snapshot, a subvolume in a subdirectory, snapshot placeholders; the second has `set-default` pointing at a subvolume |
| `n4k`, `n64k`, `s16k` | node/sector sizes 4 KiB, 64 KiB, 16 KiB |
| `meta-single`, `data-dup`, `mixed` | metadata single, data DUP, mixed block groups |
| `no-holes-off`, `no-skinny`, `space-cache-v1`, `block-group-tree`, `squota` | feature variants (`-O`) |
| `nodatasum` | mounted with `nodatasum`: no data checksums |
| `comp-zlib`, `comp-lzo`, `comp-zstd` | files written under `compress-force=...` (flagged `compressed` in the manifest) next to plain files written after compression was switched off |
| `csum-xxhash`, `csum-sha256`, `csum-blake2` | `--csum`: refused at mount |
| `raid1`, `raid0`, `raid5`, `single2dev` | the FIRST device only of a 2/2/3/2-device filesystem: refused at mount |

Whole set: about 900 KB compressed. Extent-tree-v2 and raid-stripe-tree cannot
be built (this btrfs-progs has no experimental features); they and the damaged
variants (bad superblock magic/checksum, bad tree-block checksum, chunk-tree
corruption, truncated device, unknown incompat bits, log tree) are made from good
images at test time by `tools/btrfs/corrupt.py`, not stored.

Manifest caveats: the mtime of a directory with inode 2 (Linux's synthesised
placeholder for a subvolume that was nested in a snapshot's source) and the
directory size/link count of `s16k` (tmpfs numbers) are recorded as `-` and not
compared.
